#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host: let the VR compositors outrank the game on the GPU (and run their threads realtime), so a
# frame always reaches the headset on time even when the game saturates the GPU (it then just runs
# slower, which shows far less than late or repeated frames).
#
# Monado asks for a high/realtime-priority GPU queue and realtime threads but falls back to normal
# priority without CAP_SYS_NICE ("Created device and COMPUTE queue with QUEUE_GLOBAL_PRIORITY_MEDIUM",
# "Could not raise priority for thread"). SteamVR's compositor needs the same; SteamVR asks for it
# itself on first run ("SteamVR requires superuser access").
#
# A rebuild replaces monado-service and drops the capability: run this again after building
# (build-monado-fedora.sh does; start-monado.sh warns when it's missing).
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
# Meant to run as the user (it calls sudo itself); under sudo, HOME is root's and Steam isn't found.
if [ "$(id -u)" = 0 ] && [ -n "${SUDO_USER:-}" ]; then
	HOME=$(getent passwd "$SUDO_USER" | cut -d: -f6)
	unset XDG_CONFIG_HOME
fi
. ./steam-env.sh

grant() {
	if [ -f "$1" ]; then
		sudo setcap cap_sys_nice+ep "$1"
		echo "CAP_SYS_NICE: $(getcap "$1")"
	else
		echo "Not found (skipped): $1"
	fi
}

grant external/monado/build-fedora/src/xrt/targets/service/monado-service
if [ "${1:-}" != "--monado-only" ]; then
	grant "$STEAM_ROOT/steamapps/common/SteamVR/bin/linux64/vrcompositor-launcher"
fi
