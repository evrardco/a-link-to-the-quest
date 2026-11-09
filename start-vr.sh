#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# One-click VR on the Fedora host: start Monado, wait until Quest Link is accepted in the headset,
# then start SteamVR; when SteamVR exits, stop Monado (which also restores the PC's audio output).
#
# Why this order: Monado only opens its socket once Link is accepted on the headset, and monadolink
# gets Monado's display best when it connects as SteamVR starts (it also connects later, but SteamVR
# may keep the placeholder display then). So SteamVR is started for you at the right moment. Progress shows as desktop notifications; Monado's log goes to
# monado-host.log. Install the desktop entry with ./install-vr-launcher.sh.
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")"

. ./steam-env.sh

notify() {
	echo "$*"
	if command -v notify-send >/dev/null; then
		notify-send --app-name="Quest VR" --icon=input-gaming "Quest VR" "$*" || true
	fi
}

if pgrep -x vrserver >/dev/null; then
	notify "SteamVR is already running: close it first."
	exit 1
fi

# monadolink needs SteamVR as the OpenVR runtime. An earlier xrizer setup (OpenVR games straight into
# Monado, no SteamVR) leaves openvrpaths.vrpath pointing at xrizer, read-only: put SteamVR's back
# (its backup, or no file: SteamVR recreates it when it starts).
VRPATH=$STEAM_CONFIG_HOME/openvr/openvrpaths.vrpath
if [ -f "$VRPATH" ] && grep -q xrizer "$VRPATH"; then
	chmod u+w "$VRPATH"
	if [ -f "$VRPATH.steamvr-backup" ]; then
		cp "$VRPATH.steamvr-backup" "$VRPATH"
	else
		rm "$VRPATH"
	fi
	notify "OpenVR runtime switched from xrizer back to SteamVR."
fi

SOCKET=${XDG_RUNTIME_DIR:-/tmp}/monado_comp_ipc
MONADO_PID=""
stop_monado() {
	if [ -n "$MONADO_PID" ] && kill -0 "$MONADO_PID" 2>/dev/null; then
		# TERM, not INT: background jobs of a non-interactive script ignore SIGINT, and monado-service
		# has no signal handlers (it ends on input on stdin), so INT would do nothing here. Both end
		# it the same way.
		pkill -TERM -x monado-service
		wait "$MONADO_PID" 2>/dev/null # start-monado.sh restores the audio output on its way out
	fi
}

if pgrep -x monado-service >/dev/null; then
	echo "Monado is already running: using it."
else
	marker=$(mktemp)
	trap 'stop_monado; rm -f "$marker"' EXIT
	trap 'exit 130' INT TERM HUP
	# Keep the previous session's log: starting again shouldn't wipe what's being looked at.
	[ -f monado-host.log ] && mv -f monado-host.log monado-host.prev.log
	./start-monado.sh >monado-host.log 2>&1 </dev/null &
	MONADO_PID=$!

	if ! grep -qx 2833 /sys/bus/usb/devices/*/idVendor 2>/dev/null; then
		notify "Plug in the Quest (USB-C) and wake it up."
	fi
	notify "Put on the headset and accept Quest Link."

	# Monado (re)creates its socket once Link is accepted; a stale file from an earlier run is older.
	until [ -S "$SOCKET" ] && [ "$SOCKET" -nt "$marker" ]; do
		if ! kill -0 "$MONADO_PID" 2>/dev/null; then
			notify "Monado stopped before Quest Link was accepted: see monado-host.log."
			exit 1
		fi
		sleep 0.5
	done
fi

notify "Quest Link connected: starting SteamVR."
if [ "$STEAM_KIND" = flatpak ]; then
	setsid -f flatpak run "$FLATPAK_ID" steam://rungameid/250820 >/dev/null 2>&1
else
	setsid -f steam steam://rungameid/250820 >/dev/null 2>&1
fi

# Wait for SteamVR to come up (Steam may have to start first), then for it to exit.
for _ in $(seq 240); do
	pgrep -x vrserver >/dev/null && break
	sleep 0.5
done
if ! pgrep -x vrserver >/dev/null; then
	notify "SteamVR didn't start within 2 minutes: stopping Monado."
	exit 1
fi
while pgrep -x vrserver >/dev/null; do
	sleep 2
done

if [ -n "$MONADO_PID" ]; then
	notify "SteamVR closed: stopping Monado."
fi
# The EXIT trap stops Monado (only if this script started it).
