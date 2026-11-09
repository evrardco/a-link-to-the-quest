#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host: add "Quest VR" to the desktop's application menu. It opens the Quest VR tray icon
# (vr-tray.py) and starts VR: Monado, then SteamVR once Quest Link is accepted in the headset.
#   ./install-vr-launcher.sh              menu entry
#   ./install-vr-launcher.sh --autostart  menu entry + the tray icon at login (idle until clicked)
#   ./install-vr-launcher.sh --uninstall  remove both
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

DATA=${XDG_DATA_HOME:-$HOME/.local/share}
APPS=$DATA/applications
DESKTOP=$APPS/quest-vr.desktop
AUTOSTART=${XDG_CONFIG_HOME:-$HOME/.config}/autostart/quest-vr-tray.desktop

if [ "${1:-}" = "--uninstall" ]; then
	rm -f "$DESKTOP" "$AUTOSTART"
	echo "Removed $DESKTOP and $AUTOSTART"
	exit 0
fi

if ! python3 -c "import PyQt6" 2>/dev/null && ! python3 -c "import PySide6" 2>/dev/null; then
	echo "The tray icon needs Qt for Python: sudo dnf install python3-pyqt6" >&2
	exit 1
fi

# SteamVR's own icon if Steam installed one, else a generic one.
ICON=input-gaming
if ls "$DATA"/icons/hicolor/*/apps/steam_icon_250820.png >/dev/null 2>&1; then
	ICON=steam_icon_250820
fi

entry() { # $1 = arguments for vr-tray.py, $2 = comment
	cat <<EOF
[Desktop Entry]
Type=Application
Name=Quest VR
Comment=$2
Exec=$PWD/vr-tray.py $1
Icon=$ICON
Terminal=false
Categories=Game;
EOF
}

chmod +x start-vr.sh vr-tray.py
mkdir -p "$APPS"
entry --start "Start Monado, then SteamVR once Quest Link is accepted in the headset" >"$DESKTOP"
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" >/dev/null 2>&1 || true
echo "Installed $DESKTOP: \"Quest VR\" in your application menu."

if [ "${1:-}" = "--autostart" ]; then
	mkdir -p "$(dirname "$AUTOSTART")"
	entry "" "Quest VR tray icon (click it to start VR)" >"$AUTOSTART"
	echo "Installed $AUTOSTART: the tray icon appears at login."
fi
