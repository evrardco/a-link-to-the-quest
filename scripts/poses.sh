#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host, while SteamVR runs: ask SteamVR (its vrcmd tool) which poses it has for the tracked devices,
# for 20 s, while you turn the headset in your hands. Writes <project>/poses.txt.
cd "$(dirname "$0")/../.."
STEAMVR=""
for d in ~/.local/share/Steam/steamapps/common/SteamVR ~/.steam/steam/steamapps/common/SteamVR \
	~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/SteamVR; do
	[ -d "$d" ] && { STEAMVR=$d; break; }
done
[ -n "$STEAMVR" ] || { echo "SteamVR folder not found" >&2; exit 1; }
VRCMD=$(find "$STEAMVR/bin" -type f \( -name vrcmd -o -name 'vrcmd*' \) 2>/dev/null | grep -v '\.exe$' | grep linux64 | head -1)
[ -n "$VRCMD" ] || { echo "vrcmd not found under $STEAMVR/bin; files there:" >&2; ls "$STEAMVR/bin/linux64" | head -40 >&2; exit 1; }
echo "Using $VRCMD"
echo "Turn the headset slowly in your hands for 20 s from now..."
cd "$(dirname "$VRCMD")"
LD_LIBRARY_PATH="$PWD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" timeout 20 "$VRCMD" --pollposes > "$OLDPWD/poses.txt" 2>&1
echo "exit $? (124 = stopped after 20 s, normal); $(wc -l < "$OLDPWD/poses.txt") lines in $OLDPWD/poses.txt"
head -5 "$OLDPWD/poses.txt"
