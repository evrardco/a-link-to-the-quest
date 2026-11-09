# SPDX-License-Identifier: MIT
# Sourced by the Steam/VR setup scripts: work out which Steam is in use and where its files live.
# Prefers native (RPM) Steam when present; set STEAM_KIND=flatpak or STEAM_KIND=native to override.
#
# Sets: STEAM_KIND (native|flatpak), STEAM_ROOT (Steam's data dir), STEAM_CONFIG_HOME (the XDG
# config dir Steam's games see), FLATPAK_ID.

FLATPAK_ID=com.valvesoftware.Steam

if [ -z "${STEAM_KIND:-}" ]; then
	if [ -d "$HOME/.local/share/Steam/steamapps" ] || rpm -q steam >/dev/null 2>&1; then
		STEAM_KIND=native
	elif flatpak info "$FLATPAK_ID" >/dev/null 2>&1; then
		STEAM_KIND=flatpak
	else
		STEAM_KIND=native
	fi
fi

case "$STEAM_KIND" in
native)
	STEAM_ROOT=$HOME/.local/share/Steam
	STEAM_CONFIG_HOME=${XDG_CONFIG_HOME:-$HOME/.config}
	;;
flatpak)
	STEAM_ROOT=$HOME/.var/app/$FLATPAK_ID/.local/share/Steam
	STEAM_CONFIG_HOME=$HOME/.var/app/$FLATPAK_ID/config # Flatpak redirects ~/.config here
	;;
*)
	echo "STEAM_KIND must be 'native' or 'flatpak' (got '$STEAM_KIND')" >&2
	return 1 2>/dev/null || exit 1
	;;
esac
