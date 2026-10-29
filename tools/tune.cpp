// Live tuning of the controller offset while SteamVR runs: changes the driver_monadolink settings
// through SteamVR's own IVRSettings (the driver re-reads them twice a second), and SteamVR saves them
// to steamvr.vrsettings when it exits.
//
// Usage: monadolink_tune                 interactive: single keys nudge the offset, see the help
//        monadolink_tune show            print the current values
//        monadolink_tune KEY VALUE       set one, e.g. controller_offset_pitch 12
//        monadolink_tune reset           back to the driver's defaults
#include <openvr.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <termios.h>
#include <unistd.h>

static const char *k_section = "driver_monadolink";

struct Knob
{
	const char *key;
	const char *label;
	char down, up; //!< keys
	float step;
	const char *unit;
};

// Pitch up = aim up. Position: OpenXR grip axes (+X right for the left hand, mirrored for the right;
// +Y up; -Z forward along the controller).
static const Knob k_knobs[] = {
    {"controller_offset_pitch", "pitch (aim up/down)", 's', 'w', 1.f, "deg"},
    {"controller_offset_yaw", "yaw (aim left/right)", 'd', 'a', 1.f, "deg"},
    {"controller_offset_roll", "roll (twist)", 'e', 'q', 1.f, "deg"},
    {"controller_offset_z", "forward/back", 'k', 'i', -0.005f, "m"}, // 'i' moves forward = -Z
    {"controller_offset_x", "sideways", 'j', 'l', 0.005f, "m"},
    {"controller_offset_y", "up/down", 'o', 'u', 0.005f, "m"},
};

static float
get(const char *key)
{
	return vr::VRSettings()->GetFloat(k_section, key);
}

static void
set(const char *key, float value)
{
	vr::EVRSettingsError err = vr::VRSettingsError_None;
	vr::VRSettings()->SetFloat(k_section, key, value, &err);
	if (err != vr::VRSettingsError_None) {
		fprintf(stderr, "\nSetFloat(%s) failed: %s\n", key, vr::VRSettings()->GetSettingsErrorNameFromEnum(err));
	}
}

static void
print_values(const char *end)
{
	printf("pitch %+5.1f  yaw %+5.1f  roll %+5.1f deg | x %+.3f  y %+.3f  z %+.3f m%s",
	       get("controller_offset_pitch"), get("controller_offset_yaw"), get("controller_offset_roll"),
	       get("controller_offset_x"), get("controller_offset_y"), get("controller_offset_z"), end);
	fflush(stdout);
}

static void
reset()
{
	for (const auto &k : k_knobs) {
		vr::VRSettings()->RemoveKeyInSection(k_section, k.key); // the driver's default.vrsettings applies again
	}
}

static int
interactive()
{
	printf("Live controller offset (the driver applies changes within half a second):\n");
	for (const auto &k : k_knobs) {
		printf("  %c / %c  %-22s (%g %s per press)\n", k.up, k.down, k.label, std::fabs(k.step), k.unit);
	}
	printf("  0      back to the defaults\n  Enter  quit (SteamVR saves the values when it exits)\n\n");

	termios old_tio{}, tio{};
	const bool tty = tcgetattr(STDIN_FILENO, &old_tio) == 0;
	if (tty) {
		tio = old_tio;
		tio.c_lflag &= ~(ICANON | ECHO);
		tcsetattr(STDIN_FILENO, TCSANOW, &tio);
	}
	print_values("");
	for (int c; (c = getchar()) != EOF;) {
		if (c == '\n' || c == '\r' || c == 27 || c == 'x') {
			break;
		}
		if (c == '0') {
			reset();
		}
		for (const auto &k : k_knobs) {
			if (c == k.up || c == k.down) {
				const float v = get(k.key) + (c == k.up ? k.step : -k.step);
				set(k.key, std::round(v * 1000.f) / 1000.f); // no float drift after many presses
			}
		}
		printf("\r\033[K");
		print_values("");
	}
	printf("\n");
	if (tty) {
		tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
	}
	return 0;
}

int
main(int argc, char **argv)
{
	vr::EVRInitError err = vr::VRInitError_None;
	vr::VR_Init(&err, vr::VRApplication_Background); // doesn't start SteamVR, doesn't take the scene
	if (err != vr::VRInitError_None) {
		fprintf(stderr, "Can't connect to SteamVR (is it running?): %s\n",
		        vr::VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}

	int ret = 0;
	if (argc == 1) {
		ret = interactive();
	} else if (argc == 2 && strcmp(argv[1], "show") == 0) {
		print_values("\n");
	} else if (argc == 2 && strcmp(argv[1], "reset") == 0) {
		reset();
		print_values("\n");
	} else if (argc == 3) {
		set(argv[1], (float)atof(argv[2]));
		print_values("\n");
	} else {
		fprintf(stderr, "usage: %s [show | reset | KEY VALUE]\n", argv[0]);
		ret = 2;
	}
	vr::VR_Shutdown();
	return ret;
}
