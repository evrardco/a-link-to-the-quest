#!/usr/bin/env bash
# Host: set (or show) a monadolink setting as a SteamVR user override, in steamvr.vrsettings
# (section driver_monadolink). Overrides win over the driver's defaults and survive reinstalls.
# SteamVR rewrites that file when it exits, so run this with SteamVR closed, then start SteamVR.
#
# Usage: monadolink/scripts/setting.sh                      show the current overrides
#        monadolink/scripts/setting.sh KEY VALUE            e.g. controller_offset_pitch 10
#        monadolink/scripts/setting.sh KEY --unset          back to the driver's default
set -euo pipefail
for f in ~/.local/share/Steam/config/steamvr.vrsettings \
	~/.var/app/com.valvesoftware.Steam/.local/share/Steam/config/steamvr.vrsettings; do
	[ -f "$f" ] && { FILE=$f; break; }
done
[ -n "${FILE:-}" ] || { echo "steamvr.vrsettings not found (has SteamVR run once?)" >&2; exit 1; }
if pgrep -x vrserver >/dev/null; then
	echo "SteamVR is running: close it first (it overwrites steamvr.vrsettings on exit)." >&2
	exit 1
fi
python3 - "$FILE" "$@" <<'PY'
import json, sys
path, args = sys.argv[1], sys.argv[2:]
with open(path) as f:
    data = json.load(f)
section = data.setdefault("driver_monadolink", {})
if args:
    key = args[0]
    if len(args) < 2:
        sys.exit("usage: setting.sh KEY VALUE | KEY --unset")
    raw = args[1]
    if raw == "--unset":
        section.pop(key, None)
    else:
        # numbers and booleans as JSON values, anything else as a string
        try:
            value = json.loads(raw)
        except ValueError:
            value = raw
        section[key] = value
    if not section:
        data.pop("driver_monadolink")
    with open(path, "w") as f:
        json.dump(data, f, indent=3)
        f.write("\n")
print(json.dumps(data.get("driver_monadolink", {}), indent=3))
PY
