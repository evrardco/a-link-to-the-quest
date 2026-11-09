#!/usr/bin/env bash
# Run the driver's OpenXR client against the running Monado, without SteamVR, and save the output.
# Usage (host or sandbox): a-link-to-the-quest/scripts/probe.sh [seconds]   → writes <project>/probe.txt
cd "$(dirname "$0")/../.."
if [ -f /etc/fedora-release ]; then bin=a-link-to-the-quest/build-host; json=monado/build-fedora/openxr_monado-dev.json
else bin=a-link-to-the-quest/build; json=monado/build/openxr_monado-dev.json; fi
ninja -C "$bin" monadolink_xr_probe >/dev/null || exit 1
"$bin/monadolink_xr_probe" "${1:-5}" "$PWD/$json" > probe.txt 2>&1
echo "exit $? — output in $PWD/probe.txt"; grep -m3 "Head:\|runtime\|display" probe.txt
