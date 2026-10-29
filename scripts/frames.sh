#!/usr/bin/env bash
# Host: turn the driver's frame submission (M2 Vulkan path) on or off in the installed driver.
# off = M1 behaviour (tracking only, no Vulkan). Restart SteamVR afterwards.
# Usage: monadolink/scripts/frames.sh on|off
set -euo pipefail
case "${1:-}" in on) v=true ;; off) v=false ;; *) echo "usage: $0 on|off" >&2; exit 1 ;; esac
f=$(ls -d ~/.local/share/Steam/steamapps/common/SteamVR/drivers/monadolink/resources/settings/default.vrsettings \
	~/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/SteamVR/drivers/monadolink/resources/settings/default.vrsettings \
	2>/dev/null | head -1)
[ -n "$f" ] || { echo "monadolink isn't installed (run scripts/install.sh)" >&2; exit 1; }
sed -i "s/\"submit_frames\": [a-z]*/\"submit_frames\": $v/" "$f"
grep submit_frames "$f"
