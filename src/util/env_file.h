// The Quest VR tray's settings file (~/.config/quest-vr/settings.env): KEY=VALUE lines, # comments.
// start-monado.sh sources it for Monado; monadolink reads its MONADOLINK_* keys (FSR) at Init, so the
// tray's Settings window covers the driver too.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace ml {

//! KEY=VALUE pairs of `path` (empty if it can't be read). Surrounding spaces and quotes are dropped.
inline std::map<std::string, std::string>
read_env_file(const std::string &path)
{
	std::map<std::string, std::string> out;
	FILE *f = fopen(path.c_str(), "r");
	if (f == nullptr) {
		return out;
	}
	char line[1024];
	auto trim = [](std::string s) {
		const char *ws = " \t\r\n";
		s.erase(0, s.find_first_not_of(ws));
		s.erase(s.find_last_not_of(ws) + 1);
		if (s.size() >= 2 && (s[0] == '"' || s[0] == '\'') && s.back() == s[0]) {
			s = s.substr(1, s.size() - 2);
		}
		return s;
	};
	while (fgets(line, sizeof(line), f) != nullptr) {
		std::string l = trim(line);
		const size_t eq = l.find('=');
		if (l.empty() || l[0] == '#' || eq == std::string::npos || eq == 0) {
			continue;
		}
		std::string key = trim(l.substr(0, eq));
		if (key.rfind("export ", 0) == 0) {
			key = trim(key.substr(7));
		}
		out[key] = trim(l.substr(eq + 1));
	}
	fclose(f);
	return out;
}

//! The tray's settings file: `setting` (driver_monadolink/quest_vr_settings) if set, else
//! $HOME/.config/quest-vr/settings.env (where the tray writes it on the host). Not XDG_CONFIG_HOME:
//! Flatpak Steam points that into its own sandbox.
inline std::string
quest_vr_settings_path(const std::string &setting)
{
	if (!setting.empty()) {
		return setting;
	}
	const char *home = getenv("HOME");
	return home != nullptr ? std::string(home) + "/.config/quest-vr/settings.env" : std::string();
}

//! The OpenXR runtime manifest of the Monado that's running: start-monado.sh copies its tree's
//! manifest there (setting monado_runtime_file, default ~/.cache/quest-vr/openxr_runtime.json). Its
//! client library must come from the same build as monado-service: a client from another Monado
//! fails the IPC handshake ("wrong size '0', expected '4'", 2026-11-09, old fork vs upstream).
inline std::string
monado_runtime_file_path(const std::string &setting)
{
	if (!setting.empty()) {
		return setting;
	}
	const char *home = getenv("HOME");
	return home != nullptr ? std::string(home) + "/.cache/quest-vr/openxr_runtime.json" : std::string();
}

} // namespace ml
