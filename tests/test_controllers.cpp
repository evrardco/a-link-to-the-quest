// Touch controllers (M4): OpenXR actions -> ControllerDevice -> SteamVR input components, and haptics
// back, against the fake runtime (behind the real loader) and the fake SteamVR host.
#include "device_provider.h"
#include "fake_openvr.h"
#include "fake_openxr_state.h"
#include "xr/xr_client.h"
#include "xr/xr_math.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <dlfcn.h>
#include <functional>
#include <thread>
#include <unistd.h>

static FakeXrState *
fake_runtime_state()
{
	static void *lib = dlopen(FAKE_RUNTIME_PATH, RTLD_NOW | RTLD_LOCAL);
	REQUIRE(lib != nullptr);
	auto *state = (FakeXrState *)dlsym(lib, "fake_xr_state");
	REQUIRE(state != nullptr);
	*state = FakeXrState{};
	return state;
}

static bool
eventually(const std::function<bool()> &condition)
{
	for (int i = 0; i < 300; i++) {
		if (condition()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

TEST_CASE("controller offsets: the right hand mirrors the left")
{
	const float pos[3] = {0.01f, 0.02f, -0.11f}, rot[3] = {10.f, 20.f, 30.f};
	XrPosef l = ml::controller_offset(pos, rot, false), r = ml::controller_offset(pos, rot, true);
	CHECK(l.position.x == doctest::Approx(0.01f));
	CHECK(r.position.x == doctest::Approx(-0.01f));
	CHECK(r.position.y == doctest::Approx(0.02f));
	CHECK(r.position.z == doctest::Approx(-0.11f));
	// Mirroring across the YZ plane: (w, x, y, z) -> (w, x, -y, -z).
	CHECK(r.orientation.w == doctest::Approx(l.orientation.w));
	CHECK(r.orientation.x == doctest::Approx(l.orientation.x));
	CHECK(r.orientation.y == doctest::Approx(-l.orientation.y));
	CHECK(r.orientation.z == doctest::Approx(-l.orientation.z));

	const float zero[3] = {0.f, 0.f, 0.f};
	XrPosef id = ml::controller_offset(zero, zero, true);
	CHECK(id.orientation.w == doctest::Approx(1.f));
}

TEST_CASE("sync_hands reads buttons, analogs and the grip pose with its offset")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime_state();
	s->hands[1].trigger = 0.8f;
	s->hands[1].lower_click = true;
	s->hands[1].stick_x = -0.5f;
	s->hands[0].upper_touch = true;
	s->hands[0].menu_click = true;
	s->hands[0].squeeze = 0.3f;

	ml::XrClient xr;
	REQUIRE(xr.start(FAKE_RUNTIME_JSON));
	REQUIRE(xr.input_ready());
	CHECK(s->attached);
	CHECK(s->suggested_bindings == 27);

	const float pos[3] = {0.f, 0.f, -0.11f}, rot[3] = {0.f, 0.f, 0.f};
	const XrPosef offset[2] = {ml::controller_offset(pos, rot, false), ml::controller_offset(pos, rot, true)};
	ml::HandState h[2];
	REQUIRE(xr.sync_hands(offset, h));
	CHECK(h[1].active);
	CHECK(h[1].trigger == doctest::Approx(0.8f));
	CHECK(h[1].lower_click);
	CHECK(h[1].stick_x == doctest::Approx(-0.5f));
	CHECK_FALSE(h[1].menu_click);
	CHECK(h[0].upper_touch);
	CHECK(h[0].menu_click);
	CHECK(h[0].squeeze == doctest::Approx(0.3f));
	// Grip at (0, 1.2, -0.3) facing -Z: the offset moves the pose 11 cm further forward.
	CHECK(h[0].pose.vecPosition[2] == doctest::Approx(-0.41));
	CHECK(h[0].pose.vecPosition[1] == doctest::Approx(1.2));
	CHECK(h[0].pose.result == vr::TrackingResult_Running_OK);
}

TEST_CASE("without focus, or with a hand unbound, the hands are inactive")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime_state();
	ml::XrClient xr;
	REQUIRE(xr.start(FAKE_RUNTIME_JSON));
	const XrPosef none[2] = {{{0, 0, 0, 1}, {0, 0, 0}}, {{0, 0, 0, 1}, {0, 0, 0}}};
	ml::HandState h[2];

	s->focused = false;
	s->hands[0].trigger = 1.f;
	REQUIRE(xr.sync_hands(none, h));
	CHECK_FALSE(h[0].active);
	CHECK(h[0].trigger == 0.f);

	s->focused = true;
	s->hands[1].active = false;
	REQUIRE(xr.sync_hands(none, h));
	CHECK(h[0].active);
	CHECK_FALSE(h[1].active);
}

TEST_CASE("bindings are retried without thumbrest/menu if the runtime refuses them")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime_state();
	s->reject_optional_bindings = true;
	ml::XrClient xr;
	REQUIRE(xr.start(FAKE_RUNTIME_JSON));
	CHECK(xr.input_ready());
	CHECK(s->suggest_calls == 2);
	CHECK(s->suggested_bindings == 24); // 27 minus 2 thumbrest and the menu button
}

TEST_CASE("with Monado, two Oculus Touch controllers are added and fed from the tracking thread")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	FakeXrState *s = fake_runtime_state();

	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	REQUIRE(ctx.host.added.size() == 3);
	CHECK(ctx.host.added[1].serial == "MONADOLINK-0001_Controller_Left");
	CHECK(ctx.host.added[2].serial == "MONADOLINK-0001_Controller_Right");
	CHECK(ctx.host.added[1].device_class == vr::TrackedDeviceClass_Controller);

	auto *left = ctx.host.added[1].driver, *right = ctx.host.added[2].driver;
	REQUIRE(left->Activate(1) == vr::VRInitError_None);
	REQUIRE(right->Activate(2) == vr::VRInitError_None);
	const auto lc = ctx.properties.TrackedDeviceToPropertyContainer(1);
	const auto rc = ctx.properties.TrackedDeviceToPropertyContainer(2);

	CHECK(fake::get_property<int32_t>(ctx.properties, lc, vr::Prop_ControllerRoleHint_Int32) ==
	      vr::TrackedControllerRole_LeftHand);
	CHECK(fake::get_property<int32_t>(ctx.properties, rc, vr::Prop_ControllerRoleHint_Int32) ==
	      vr::TrackedControllerRole_RightHand);
	CHECK(fake::get_string_property(ctx.properties, rc, vr::Prop_ControllerType_String) == "oculus_touch");
	CHECK(fake::get_string_property(ctx.properties, rc, vr::Prop_InputProfilePath_String) ==
	      "{oculus}/input/touch_profile.json");
	CHECK(fake::get_string_property(ctx.properties, lc, vr::Prop_RenderModelName_String) ==
	      "oculus_quest2_controller_left");

	// Oculus Touch layout: X/Y + menu on the left, A/B on the right.
	CHECK(ctx.input.find(lc, "/input/x/click") != nullptr);
	CHECK(ctx.input.find(lc, "/input/system/click") != nullptr);
	CHECK(ctx.input.find(lc, "/input/a/click") == nullptr);
	CHECK(ctx.input.find(rc, "/input/a/click") != nullptr);
	CHECK(ctx.input.find(rc, "/input/b/touch") != nullptr);
	CHECK(ctx.input.find(rc, "/input/system/click") == nullptr);
	REQUIRE(ctx.input.find(rc, "/input/joystick/x") != nullptr);
	CHECK(ctx.input.find(rc, "/input/joystick/x")->units == vr::VRScalarUnits_NormalizedTwoSided);
	CHECK(ctx.input.find(rc, "/input/trigger/value")->units == vr::VRScalarUnits_NormalizedOneSided);

	// Pressing things on the "Quest" shows up in SteamVR.
	s->hands[1].trigger = 0.9f;
	s->hands[1].lower_click = true;
	s->hands[0].stick_y = 1.f;
	CHECK(eventually([&] { return ctx.input.value(rc, "/input/trigger/value") > 0.89f; }));
	CHECK(ctx.input.value(rc, "/input/trigger/click") == 1.f); // above the click threshold
	CHECK(ctx.input.value(rc, "/input/a/click") == 1.f);
	CHECK(ctx.input.value(rc, "/input/a/touch") == 1.f); // a click implies a touch
	CHECK(eventually([&] { return ctx.input.value(lc, "/input/joystick/y") == 1.f; }));

	// Poses go to both controller devices.
	CHECK(eventually([&] {
		bool l = false, r = false;
		for (const auto &[device, pose] : ctx.host.poses_copy()) {
			l |= device == 1 && pose.poseIsValid;
			r |= device == 2 && pose.poseIsValid;
		}
		return l && r;
	}));

	// Unchanged values aren't re-sent at 500 Hz.
	vr::VRInputComponentHandle_t trigger_handle = 0;
	ctx.input.find(rc, "/input/trigger/value", &trigger_handle);
	int updates_before;
	{
		std::lock_guard lock(ctx.input.mutex);
		updates_before = ctx.input.components[trigger_handle].updates;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	{
		std::lock_guard lock(ctx.input.mutex);
		CHECK(ctx.input.components[trigger_handle].updates == updates_before);
	}

	// Haptics: SteamVR's event for the right controller's haptic component vibrates the right hand.
	vr::VRInputComponentHandle_t haptic = 0;
	REQUIRE(ctx.input.find(rc, "/output/haptic", &haptic) != nullptr);
	vr::VREvent_t ev{};
	ev.eventType = vr::VREvent_Input_HapticVibration;
	ev.data.hapticVibration.containerHandle = rc;
	ev.data.hapticVibration.componentHandle = haptic;
	ev.data.hapticVibration.fDurationSeconds = 0.05f;
	ev.data.hapticVibration.fFrequency = 160.f;
	ev.data.hapticVibration.fAmplitude = 0.7f;
	{
		std::lock_guard lock(ctx.host.mutex);
		ctx.host.events.push_back(ev);
	}
	provider.RunFrame();
	CHECK(s->haptic_calls == 1);
	CHECK(s->haptic_hand == 1);
	CHECK(s->haptic_amplitude == doctest::Approx(0.7f));
	CHECK(s->haptic_frequency == doctest::Approx(160.f));
	CHECK(s->haptic_duration == 50'000'000);

	left->Deactivate();
	right->Deactivate();
	provider.Cleanup();
	CHECK(s->instances == 0);
}

TEST_CASE("controllers can be turned off in the settings")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	ctx.settings.SetBool("driver_monadolink", "controllers", false);
	fake_runtime_state();
	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	CHECK(ctx.host.added.size() == 1);
	provider.Cleanup();
}

TEST_CASE("controller offsets are picked up live: a changed setting reaches the poses without a restart")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	ctx.settings.SetFloat("driver_monadolink", "controller_offset_z", 0.f); // pure rotation for this test
	ctx.settings.SetFloat("driver_monadolink", "controller_offset_pitch", 0.f);
	fake_runtime_state(); // grips face -Z with no rotation
	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	REQUIRE(ctx.host.added.size() == 3);
	ctx.host.added[2].driver->Activate(2);
	CHECK_FALSE(provider.reload_controller_offsets()); // nothing changed since Init

	auto right_pitch_deg = [&] {
		vr::DriverPose_t last{};
		for (const auto &[device, pose] : ctx.host.poses_copy()) {
			if (device == 2) {
				last = pose;
			}
		}
		return 2.0 * std::asin(last.qRotation.x) * 180.0 / M_PI; // a pure X rotation
	};
	REQUIRE(eventually([&] { return !ctx.host.poses_copy().empty(); }));
	CHECK(right_pitch_deg() == doctest::Approx(0.0).epsilon(0.01));

	// What tools/tune does through IVRSettings; RunFrame re-reads within half a second.
	ctx.settings.SetFloat("driver_monadolink", "controller_offset_pitch", 12.f);
	std::this_thread::sleep_for(std::chrono::milliseconds(600));
	provider.RunFrame();
	CHECK(eventually([&] { return std::fabs(right_pitch_deg() - 12.0) < 0.1; }));
	bool logged = false;
	for (const auto &l : ctx.log.copy()) {
		logged |= l.find("Controller grip offset") != std::string::npos && l.find("pitch 12.0") != std::string::npos;
	}
	CHECK(logged);

	ctx.host.added[2].driver->Deactivate();
	provider.Cleanup();
}

TEST_CASE("in-headset tuning: the chord, a flick, haptics, hidden buttons, saved settings and A for defaults")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	FakeXrState *s = fake_runtime_state();
	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	REQUIRE(ctx.host.added.size() == 3);
	ctx.host.added[1].driver->Activate(1);
	ctx.host.added[2].driver->Activate(2);
	const auto rc = ctx.properties.TrackedDeviceToPropertyContainer(2);

	// Hold both sticks clicked: tuning starts after a second, with a buzz on both hands.
	s->hands[0].stick_click = s->hands[1].stick_click = true;
	REQUIRE(eventually([&] { return s->haptic_calls >= 2; }));
	CHECK(std::llabs(s->haptic_duration - 400'000'000) < 1000);
	s->hands[0].stick_click = s->hands[1].stick_click = false;

	// While tuning the game sees nothing: the stick and the trigger stay at rest.
	s->hands[1].trigger = 1.f;
	s->hands[1].stick_y = 1.f; // flick: pitch +1
	REQUIRE(eventually([&] { return s->haptic_calls >= 3; }));
	CHECK(std::llabs(s->haptic_duration - 20'000'000) < 1000);
	CHECK(s->haptic_hand == 1);
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	CHECK(ctx.input.value(rc, "/input/joystick/y") != 1.f);
	CHECK(ctx.input.value(rc, "/input/trigger/value") != 1.f);
	s->hands[1].stick_y = 0.f;
	s->hands[1].trigger = 0.f;

	// RunFrame saves it to the settings (SteamVR persists them), and the reload doesn't undo it.
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	provider.RunFrame();
	CHECK(ctx.settings.GetFloat("driver_monadolink", "controller_offset_pitch") == doctest::Approx(23.f)); // 22 + 1
	CHECK(ctx.settings.GetFloat("driver_monadolink", "controller_offset_z") == doctest::Approx(-0.08f));
	CHECK_FALSE(provider.reload_controller_offsets());

	// A: back to the defaults.
	s->hands[1].lower_click = true;
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	s->hands[1].lower_click = false;
	provider.RunFrame();
	CHECK(ctx.settings.GetFloat("driver_monadolink", "controller_offset_pitch") == doctest::Approx(22.f)); // default
	CHECK(ctx.input.value(rc, "/input/a/click") != 1.f); // A was the tuner's, not the game's

	// Leave: the game gets its input back.
	const int calls = s->haptic_calls;
	s->hands[0].stick_click = s->hands[1].stick_click = true;
	REQUIRE(eventually([&] { return s->haptic_calls >= calls + 2; }));
	CHECK(std::llabs(s->haptic_duration - 100'000'000) < 1000);
	s->hands[0].stick_click = s->hands[1].stick_click = false;
	s->hands[1].trigger = 1.f;
	CHECK(eventually([&] { return ctx.input.value(rc, "/input/trigger/value") == 1.f; }));

	ctx.host.added[1].driver->Deactivate();
	ctx.host.added[2].driver->Deactivate();
	provider.Cleanup();
}

TEST_CASE("in-headset tuning can be turned off in the settings")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	ctx.settings.SetBool("driver_monadolink", "controller_tuning", false);
	FakeXrState *s = fake_runtime_state();
	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	ctx.host.added[2].driver->Activate(2);
	const auto rc = ctx.properties.TrackedDeviceToPropertyContainer(2);
	s->hands[0].stick_click = s->hands[1].stick_click = true;
	std::this_thread::sleep_for(std::chrono::milliseconds(1300));
	CHECK(s->haptic_calls == 0);
	CHECK(ctx.input.value(rc, "/input/joystick/click") == 1.f); // the game keeps its sticks
	ctx.host.added[2].driver->Deactivate();
	provider.Cleanup();
}

TEST_CASE("in-headset mode: B touches the file Monado watches for a stream reset")
{
	char dir[] = "/tmp/monadolink-test-XXXXXX";
	REQUIRE(mkdtemp(dir) != nullptr);
	const std::string trigger = std::string(dir) + "/quest-vr/reset-stream"; // parent created on demand

	fake::DriverContext ctx;
	ctx.install();
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	ctx.settings.SetString("driver_monadolink", "stream_reset_file", trigger.c_str());
	FakeXrState *s = fake_runtime_state();
	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	ctx.host.added[2].driver->Activate(2);

	s->hands[0].stick_click = s->hands[1].stick_click = true; // enter the in-headset mode
	REQUIRE(eventually([&] { return s->haptic_calls >= 2; }));
	s->hands[0].stick_click = s->hands[1].stick_click = false;
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	CHECK(access(trigger.c_str(), F_OK) != 0);
	s->hands[1].upper_click = true; // B
	CHECK(eventually([&] { return access(trigger.c_str(), F_OK) == 0; }));
	s->hands[1].upper_click = false;
	bool logged = false;
	for (const auto &l : ctx.log.copy()) {
		logged |= l.find("Stream reset requested") != std::string::npos;
	}
	CHECK(logged);

	ctx.host.added[2].driver->Deactivate();
	provider.Cleanup();
	unlink(trigger.c_str());
	rmdir((std::string(dir) + "/quest-vr").c_str());
	rmdir(dir);
}
