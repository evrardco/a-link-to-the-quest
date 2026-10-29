#!/usr/bin/env bash
# Build monadolink and install it into SteamVR's drivers folder (Flatpak or native Steam).
# Usage: [MONADOLINK_TIMING_STATS_S=N] scripts/install.sh            build on the host (build-host/), copy
#        scripts/install.sh --link                                   link SteamVR to the sandbox's build/
#        scripts/install.sh --uninstall
# --link (once): SteamVR's drivers/monadolink becomes a symlink to build/monadolink, so every sandbox
# `ninja -C build` is the install; restart SteamVR to load it. Running without --link replaces the
# link with a copy of a host build again.
# Run on the Fedora host (SteamVR lives there), not in the sandbox.
set -euo pipefail
cd "$(dirname "$0")/.."

DRIVER=monadolink

# Find SteamVR: check every Steam library listed in libraryfolders.vdf, for Flatpak and native Steam.
find_steamvr() {
	# Native (RPM) Steam first, then Flatpak; STEAM_ROOT overrides the search.
	local steam_roots=(
		${STEAM_ROOT:+"$STEAM_ROOT"}
		"$HOME/.local/share/Steam"
		"$HOME/.steam/steam"
		"$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
	)
	local root vdf lib
	for root in "${steam_roots[@]}"; do
		vdf="$root/steamapps/libraryfolders.vdf"
		[ -f "$vdf" ] || continue
		for lib in "$root" $(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"/\1/p' "$vdf"); do
			if [ -d "$lib/steamapps/common/SteamVR/drivers" ]; then
				echo "$lib/steamapps/common/SteamVR"
				return 0
			fi
		done
	done
	return 1
}

STEAMVR=$(find_steamvr) || { echo "SteamVR not found; install it from Steam first." >&2; exit 1; }
DEST="$STEAMVR/drivers/$DRIVER"

if [ "${1:-}" = "--uninstall" ]; then
	rm -rf "$DEST"
	echo "Removed $DEST"
	exit 0
fi

# The Monado manifest is baked into the build's default.vrsettings (MONADOLINK_OPENXR_RUNTIME_JSON,
# default this project's monado/build-fedora); MONADO_RUNTIME_JSON overrides it for a host build.
check_install() {
	local json
	json=$(sed -n 's/.*"openxr_runtime_json": "\(.*\)".*/\1/p' "$DEST/resources/settings/default.vrsettings")
	if [ -f "$json" ]; then
		echo "OpenXR runtime: $json"
	else
		echo "WARNING: $json not found: build Monado (build-monado-fedora.sh) or set MONADO_RUNTIME_JSON." >&2
	fi
	# The controllers present themselves as Oculus Touch and borrow SteamVR's own Oculus input profile
	# and Quest 2 render models.
	[ -f "$STEAMVR/drivers/oculus/resources/input/touch_profile.json" ] ||
		echo "WARNING: $STEAMVR/drivers/oculus/resources/input/touch_profile.json is missing: set controller_input_profile." >&2
	[ -d "$STEAMVR/resources/rendermodels/oculus_quest2_controller_left" ] ||
		echo "WARNING: SteamVR has no oculus_quest2_controller_left render model: controllers will be invisible." >&2
	echo
	echo "SteamVR must be the OpenVR runtime (not xrizer): run ./openvr-runtime.sh steamvr in the project folder."
	echo "Tune the controller offset in the headset (hold both sticks clicked 1 s) or live from a terminal:"
	echo "  $1/monadolink_tune"
	echo "Logs: ~/.local/share/Steam/logs/vrserver.txt (Flatpak Steam: ~/.var/app/com.valvesoftware.Steam/.local/share/Steam/logs);"
	echo "      grep for [monadolink]."
}

if [ "${1:-}" = "--link" ]; then
	TARGET="$PWD/build/$DRIVER"
	[ -f "$TARGET/bin/linux64/driver_$DRIVER.so" ] ||
		{ echo "$TARGET has no driver yet: build it in the sandbox first (ninja -C monadolink/build)." >&2; exit 1; }
	rm -rf "$DEST"
	ln -s "$TARGET" "$DEST"
	echo "Linked $DEST -> $TARGET"
	echo "Sandbox builds now install themselves: restart SteamVR after a rebuild."
	check_install "$PWD/build"
	exit 0
fi

# Host builds go to build-host/: build/ is the sandbox's (the project folder is shared, and the two
# toolchains must not share a CMake cache).
BUILD_DIR=build-host
git submodule update --init --depth 1
# MONADOLINK_TIMING_STATS_S=N: seconds between timing lines in vrserver.txt (0 = off); kept in the
# CMake cache until changed again.
cmake -S . -B "$BUILD_DIR" -G Ninja ${MONADOLINK_TIMING_STATS_S:+-DMONADOLINK_TIMING_STATS_S=$MONADOLINK_TIMING_STATS_S} \
	${MONADO_RUNTIME_JSON:+-DMONADOLINK_OPENXR_RUNTIME_JSON=$MONADO_RUNTIME_JSON}
ninja -C "$BUILD_DIR"

rm -rf "$DEST" # also just removes a --link symlink, not its target
cp -a "$BUILD_DIR/$DRIVER" "$DEST"
echo "Installed to $DEST"
check_install "$PWD/$BUILD_DIR"
