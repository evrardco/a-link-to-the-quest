// SPDX-License-Identifier: MIT
// XrClient and DeviceProvider against the real (static) OpenXR loader + tests/fake_openxr_runtime.cpp.
#include "device_provider.h"
#include "fake_openvr.h"
#include "fake_openxr_state.h"
#include "xr/xr_client.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <ctime>
#include <dlfcn.h>
#include <functional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

//! Keeps the fake runtime loaded (the loader unloads it with each instance) and resets its state.
static FakeXrState *
fake_runtime()
{
	static void *lib = dlopen(FAKE_RUNTIME_PATH, RTLD_NOW | RTLD_LOCAL);
	REQUIRE_MESSAGE(lib != nullptr, dlerror());
	auto *state = (FakeXrState *)dlsym(lib, "fake_xr_state");
	REQUIRE(state != nullptr);
	*state = FakeXrState{};
	return state;
}

//! A listening unix socket standing in for Monado's (the driver only checks that it's there).
struct FakeMonadoSocket
{
	std::string path;
	int fd = -1;
	explicit FakeMonadoSocket(const std::string &p) : path(p)
	{
		unlink(path.c_str());
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		sockaddr_un addr{};
		addr.sun_family = AF_UNIX;
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
		REQUIRE(bind(fd, (sockaddr *)&addr, sizeof(addr)) == 0);
		REQUIRE(listen(fd, 1) == 0);
	}
	~FakeMonadoSocket()
	{
		close(fd);
		unlink(path.c_str());
	}
};

static int64_t
monotonic_ns()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

TEST_CASE("XrClient fails cleanly when no runtime is reachable")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::XrClient xr;
	CHECK_FALSE(xr.start("/nonexistent/runtime.json"));
	vr::DriverPose_t pose{};
	CHECK_FALSE(xr.locate_head(&pose));
}

TEST_CASE("XrClient refuses a runtime without headless sessions")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime();
	s->support_headless = false;

	ml::XrClient xr;
	CHECK_FALSE(xr.start(FAKE_RUNTIME_JSON));
	CHECK(s->instances == 0);

	// The loader keeps a runtime loaded, with the extension list it read, from
	// xrEnumerateInstanceExtensionProperties until an instance is created and destroyed: cycle a bare
	// instance so the next tests see a fresh runtime.
	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "reset");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	XrInstance instance = XR_NULL_HANDLE;
	REQUIRE(xrCreateInstance(&ici, &instance) == XR_SUCCESS);
	xrDestroyInstance(instance);
}

TEST_CASE("XrClient starts a headless session, begins it and cleans up")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime();
	{
		ml::XrClient xr;
		REQUIRE(xr.start(FAKE_RUNTIME_JSON));
		CHECK(s->instances == 1);
		CHECK(s->sessions == 1);
		CHECK(s->spaces == 4); // base + view + two grip spaces
		CHECK_FALSE(s->session_had_graphics_binding);
		CHECK(s->session_begun); // READY was handled inside start()
		CHECK_FALSE(xr.lost());
	}
	CHECK(s->instances == 0);
	CHECK(s->sessions == 0);
	CHECK(s->spaces == 0);
}

TEST_CASE("apply_display_info takes size, refresh, FOV and eye poses from the runtime")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime();
	s->width[1] = 1450; // the larger eye wins

	ml::XrClient xr;
	REQUIRE(xr.start(FAKE_RUNTIME_JSON));
	ml::DisplayConfig c = ml::default_display_config();
	xr.apply_display_info(&c);

	CHECK(c.render_width == 1450);
	CHECK(c.render_height == 1584);
	CHECK(c.refresh_rate == doctest::Approx(90.f));
	CHECK(c.tan_left[0] == doctest::Approx(std::tan(-0.90f)));
	CHECK(c.tan_right[1] == doctest::Approx(std::tan(0.90f)));
	CHECK(c.tan_top[0] == doctest::Approx(std::tan(-0.85f)));   // OpenVR top = tan(down edge)
	CHECK(c.tan_bottom[0] == doctest::Approx(std::tan(0.80f))); // OpenVR bottom = tan(up edge)
	CHECK(c.tan_bottom[1] == doctest::Approx(std::tan(0.80f))); // up edge
	CHECK(c.ipd_meters == doctest::Approx(0.064f));
	REQUIRE(c.has_eye_to_head);
	CHECK(c.eye_to_head[0].m[0][3] == doctest::Approx(-0.032f));
	CHECK(c.eye_to_head[1].m[0][3] == doctest::Approx(0.032f));
	CHECK(c.eye_to_head[0].m[1][3] == doctest::Approx(0.f)); // relative to the head, not the floor
}

TEST_CASE("without the refresh rate extension the configured rate stays")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime();
	s->support_refresh_rate = false;

	ml::XrClient xr;
	REQUIRE(xr.start(FAKE_RUNTIME_JSON));
	ml::DisplayConfig c = ml::default_display_config();
	xr.apply_display_info(&c);
	CHECK(c.refresh_rate == doctest::Approx(120.f));
}

TEST_CASE("locate_head returns the runtime's head pose in STAGE at the current monotonic time")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime();
	s->head.position = {0.1f, 1.65f, -0.3f};
	s->linear_velocity = {0.f, 0.f, -1.f};

	ml::XrClient xr;
	REQUIRE(xr.start(FAKE_RUNTIME_JSON));
	const int64_t before = monotonic_ns();
	vr::DriverPose_t pose{};
	REQUIRE(xr.locate_head(&pose));
	const int64_t after = monotonic_ns();

	CHECK(s->base_type == XR_REFERENCE_SPACE_TYPE_STAGE);
	CHECK(s->last_locate_time >= before);
	CHECK(s->last_locate_time <= after);
	CHECK(pose.poseIsValid);
	CHECK(pose.result == vr::TrackingResult_Running_OK);
	CHECK(pose.vecPosition[1] == doctest::Approx(1.65));
	CHECK(pose.vecVelocity[2] == doctest::Approx(-1.0));
	CHECK(pose.vecWorldFromDriverTranslation[1] == 0.0);
}

TEST_CASE("without STAGE the head is located in LOCAL, lifted to standing height")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime();
	s->support_stage = false;

	ml::XrClient xr;
	REQUIRE(xr.start(FAKE_RUNTIME_JSON));
	vr::DriverPose_t pose{};
	REQUIRE(xr.locate_head(&pose));
	CHECK(s->base_type == XR_REFERENCE_SPACE_TYPE_LOCAL);
	CHECK(pose.vecWorldFromDriverTranslation[1] == doctest::Approx(1.6));
}

TEST_CASE("instance loss is noticed and stops tracking")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime();

	ml::XrClient xr;
	REQUIRE(xr.start(FAKE_RUNTIME_JSON));
	s->lose_instance = true;
	xr.poll_events();
	CHECK(xr.lost());
	vr::DriverPose_t pose{};
	CHECK_FALSE(xr.locate_head(&pose));
}

TEST_CASE("after a loss, restart() brings the client back with a new instance and calls the teardown hook")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime();

	ml::XrClient xr;
	int hook_calls = 0;
	xr.set_teardown_hook([&] { hook_calls++; });
	REQUIRE(xr.start(FAKE_RUNTIME_JSON));
	s->lose_instance = true;
	xr.poll_events();
	REQUIRE(xr.lost());

	s->lose_instance = false; // Monado is back
	REQUIRE(xr.restart());
	CHECK(hook_calls == 1);
	CHECK_FALSE(xr.lost());
	CHECK_FALSE(xr.restarting());
	CHECK(xr.running());
	CHECK(s->instances == 1); // the old one was destroyed
	CHECK(s->sessions == 1);
	vr::DriverPose_t pose{};
	CHECK(xr.locate_head(&pose));
	XrPosef head{};
	CHECK(xr.last_head(&head));
}

TEST_CASE("unix_socket_listening tells a listening socket from a missing one")
{
	const std::string path = std::string(P_tmpdir) + "/monadolink-test-sock-" + std::to_string(getpid());
	CHECK_FALSE(ml::unix_socket_listening(path));
	{
		FakeMonadoSocket sock(path);
		CHECK(ml::unix_socket_listening(path));
	}
	CHECK_FALSE(ml::unix_socket_listening(path));
	CHECK(ml::monado_socket_path("/x/y") == "/x/y");
}

// --- DeviceProvider with the fake runtime -----------------------------------------------------------

static bool
logged(fake::DriverContext &ctx, const char *text);

static bool
wait_for(const std::function<bool()> &condition)
{
	for (int i = 0; i < 200; i++) {
		if (condition()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return false;
}

TEST_CASE("with a runtime, the HMD reports its display and the tracking thread publishes head poses")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	FakeXrState *s = fake_runtime();
	s->head.position = {0.f, 1.72f, 0.f};

	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	auto *hmd = ctx.host.added.at(0).driver;
	REQUIRE(hmd->Activate(2) == vr::VRInitError_None);

	const auto c = ctx.properties.TrackedDeviceToPropertyContainer(2);
	CHECK(fake::get_property<float>(ctx.properties, c, vr::Prop_DisplayFrequency_Float) == doctest::Approx(90.f));
	CHECK(fake::get_property<float>(ctx.properties, c, vr::Prop_UserIpdMeters_Float) == doctest::Approx(0.064f));
	CHECK(ctx.host.eye_to_head_calls == 1);
	CHECK(ctx.host.eye_to_head[1].m[0][3] == doctest::Approx(0.032f));
	uint32_t w = 0, h = 0;
	static_cast<vr::IVRDisplayComponent *>(hmd->GetComponent(vr::IVRDisplayComponent_Version))
	    ->GetRecommendedRenderTargetSize(&w, &h);
	CHECK(w == 1440);
	CHECK(h == 1584);

	REQUIRE(wait_for([&] { return !ctx.host.poses_copy().empty(); }));
	auto [device, pose] = ctx.host.poses_copy().back();
	CHECK(device == 2);
	CHECK(pose.poseIsValid);
	CHECK(pose.vecPosition[1] == doctest::Approx(1.72));

	// The head moves: a later pose follows.
	s->head.position.y = 1.5f;
	CHECK(wait_for([&] { return ctx.host.poses_copy().back().second.vecPosition[1] == doctest::Approx(1.5); }));

	// RunFrame doesn't add static poses when tracking is live.
	{
		std::lock_guard lock(ctx.host.mutex);
		ctx.host.poses.clear();
	}
	provider.RunFrame();
	for (const auto &[d, p] : ctx.host.poses_copy()) {
		CHECK(p.vecPosition[1] == doctest::Approx(1.5));
	}

	hmd->Deactivate();
	provider.Cleanup();
	CHECK(s->instances == 0);
}

TEST_CASE("with fsr_render_scale SteamVR is told a smaller render size (monadolink upscales)")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	ctx.settings.SetFloat("driver_monadolink", "fsr_render_scale", 0.75f);
	fake_runtime();

	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	auto *hmd = ctx.host.added.at(0).driver;
	REQUIRE(hmd->Activate(2) == vr::VRInitError_None);
	uint32_t w = 0, h = 0;
	static_cast<vr::IVRDisplayComponent *>(hmd->GetComponent(vr::IVRDisplayComponent_Version))
	    ->GetRecommendedRenderTargetSize(&w, &h);
	CHECK(w == 1080); // 1440 x 0.75
	CHECK(h == 1188); // 1584 x 0.75
	CHECK(logged(ctx, "FSR: SteamVR renders 1080x1188 per eye"));
	hmd->Deactivate();
	provider.Cleanup();
}

TEST_CASE("the tray's settings file (settings.env) overrides the FSR settings")
{
	fake::DriverContext ctx;
	ctx.install();
	const std::string env = std::string(P_tmpdir) + "/monadolink-test-settings-" + std::to_string(getpid()) + ".env";
	{
		FILE *f = fopen(env.c_str(), "w");
		REQUIRE(f != nullptr);
		fputs("# Quest VR streaming settings\nQL_OVERRIDE_FPS=90\nMONADOLINK_FSR_RENDER_SCALE=0.5\n"
		      "MONADOLINK_FSR_SHARPNESS=0.6\n",
		      f);
		fclose(f);
	}
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	ctx.settings.SetString("driver_monadolink", "quest_vr_settings", env.c_str());
	ctx.settings.SetFloat("driver_monadolink", "fsr_render_scale", 0.75f); // the file wins
	fake_runtime();

	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	auto *hmd = ctx.host.added.at(0).driver;
	REQUIRE(hmd->Activate(2) == vr::VRInitError_None);
	uint32_t w = 0, h = 0;
	static_cast<vr::IVRDisplayComponent *>(hmd->GetComponent(vr::IVRDisplayComponent_Version))
	    ->GetRecommendedRenderTargetSize(&w, &h);
	CHECK(w == 720); // 1440 x 0.5
	CHECK(h == 792);
	CHECK(logged(ctx, "sharpness 0.60"));
	CHECK(logged(ctx, "MONADOLINK_FSR_RENDER_SCALE = 0.5 (from"));
	hmd->Deactivate();
	provider.Cleanup();
	remove(env.c_str());
}

TEST_CASE("when Monado goes away the HMD is reported disconnected")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	FakeXrState *s = fake_runtime();

	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	ctx.host.added.at(0).driver->Activate(0);
	REQUIRE(wait_for([&] { return !ctx.host.poses_copy().empty(); }));

	s->lose_instance = true;
	CHECK(wait_for([&] {
		auto poses = ctx.host.poses_copy();
		return !poses.back().second.deviceIsConnected && !poses.back().second.poseIsValid;
	}));

	provider.Cleanup();
	CHECK(s->instances == 0);
}

static bool
logged(fake::DriverContext &ctx, const char *text)
{
	for (const auto &l : ctx.log.copy()) {
		if (l.find(text) != std::string::npos) {
			return true;
		}
	}
	return false;
}

TEST_CASE("when Monado comes back (its socket listens again) the HMD reconnects and tracks again")
{
	fake::DriverContext ctx;
	ctx.install();
	const std::string sock_path = std::string(P_tmpdir) + "/monadolink-test-monado-" + std::to_string(getpid());
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	ctx.settings.SetString("driver_monadolink", "monado_socket", sock_path.c_str());
	ctx.settings.SetInt32("driver_monadolink", "reconnect_interval_ms", 20);
	FakeXrState *s = fake_runtime();

	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	ctx.host.added.at(0).driver->Activate(0);
	REQUIRE(wait_for([&] { return !ctx.host.poses_copy().empty(); }));

	s->lose_instance = true;
	REQUIRE(wait_for([&] { return !ctx.host.poses_copy().back().second.deviceIsConnected; }));

	// Monado restarts: the runtime answers again, but until its socket listens nothing is tried
	// (xrCreateInstance would block for ~20 s against the real Monado).
	s->lose_instance = false;
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
	CHECK(s->instances == 1); // only the lost one, destroyed by the reconnect
	CHECK_FALSE(logged(ctx, "socket is back"));
	CHECK_FALSE(ctx.host.poses_copy().back().second.deviceIsConnected);

	{
		FakeMonadoSocket sock(sock_path);
		s->head.position = {0.f, 1.55f, 0.f};
		CHECK(wait_for([&] {
			auto p = ctx.host.poses_copy().back().second;
			return p.deviceIsConnected && p.poseIsValid && std::fabs(p.vecPosition[1] - 1.55) < 1e-3;
		}));
		CHECK(s->instances == 1);
		CHECK(logged(ctx, "Reconnected to Monado"));

		// And again: a second loss is noticed and recovered too.
		s->lose_instance = true;
		REQUIRE(wait_for([&] { return !ctx.host.poses_copy().back().second.deviceIsConnected; }));
		s->lose_instance = false;
		CHECK(wait_for([&] { return ctx.host.poses_copy().back().second.deviceIsConnected; }));
	}

	provider.Cleanup();
	CHECK(s->instances == 0);
}

TEST_CASE("Monado not there when SteamVR starts: it connects later, with controllers and Monado's display")
{
	fake::DriverContext ctx;
	ctx.install();
	const std::string sock_path = std::string(P_tmpdir) + "/monadolink-test-monado3-" + std::to_string(getpid());
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	ctx.settings.SetString("driver_monadolink", "monado_socket", sock_path.c_str());
	ctx.settings.SetInt32("driver_monadolink", "reconnect_interval_ms", 20);
	FakeXrState *s = fake_runtime();
	s->fail_create_instance = true;

	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	REQUIRE(ctx.host.added.size() == 3); // HMD + both controllers, though nothing is connected
	CHECK(logged(ctx, "Monado not reachable yet"));
	auto *hmd = ctx.host.added[0].driver;
	REQUIRE(hmd->Activate(0) == vr::VRInitError_None);
	auto *display = (vr::IVRDisplayComponent *)hmd->GetComponent(vr::IVRDisplayComponent_Version);
	uint32_t w = 0, h = 0;
	display->GetRecommendedRenderTargetSize(&w, &h);
	CHECK(w == 1832); // the placeholder from the settings
	provider.RunFrame();
	CHECK_FALSE(ctx.host.poses_copy().back().second.deviceIsConnected);

	// Monado comes up: nothing happens until its socket listens.
	s->fail_create_instance = false;
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	CHECK(s->instances == 0);
	{
		FakeMonadoSocket sock(sock_path);
		CHECK(wait_for([&] {
			auto p = ctx.host.poses_copy().back().second;
			return p.deviceIsConnected && p.poseIsValid;
		}));
		CHECK(logged(ctx, "Connected to Monado after SteamVR started"));
		CHECK_FALSE(logged(ctx, "Lost Monado"));

		// SteamVR got Monado's display: render size, projection (left eye: tan of l, r, down, up), refresh.
		display->GetRecommendedRenderTargetSize(&w, &h);
		CHECK(w == 1440);
		CHECK(h == 1584);
		CHECK(ctx.host.render_target_width == 1440);
		CHECK(ctx.host.projection_left.vTopLeft.v[0] == doctest::Approx(std::tan(-0.90f)));
		CHECK(ctx.host.projection_left.vTopLeft.v[1] == doctest::Approx(std::tan(-0.85f)));
		CHECK(ctx.host.projection_left.vBottomRight.v[1] == doctest::Approx(std::tan(0.80f)));
		float l, r, t, b;
		display->GetProjectionRaw(vr::Eye_Left, &l, &r, &t, &b);
		CHECK(l == doctest::Approx(std::tan(-0.90f)));
		const auto c = ctx.properties.TrackedDeviceToPropertyContainer(0);
		CHECK(fake::get_property<float>(ctx.properties, c, vr::Prop_DisplayFrequency_Float) == doctest::Approx(90.f));

		// The controllers are fed now.
		auto *right = ctx.host.added[2].driver;
		REQUIRE(right->Activate(2) == vr::VRInitError_None);
		CHECK(wait_for([&] {
			for (const auto &[device, pose] : ctx.host.poses_copy()) {
				if (device == 2 && pose.poseIsValid) {
					return true;
				}
			}
			return false;
		}));
	}
	provider.Cleanup();
}

TEST_CASE("a reconnect to a Monado with another display says SteamVR must be restarted to use it")
{
	fake::DriverContext ctx;
	ctx.install();
	const std::string sock_path = std::string(P_tmpdir) + "/monadolink-test-monado2-" + std::to_string(getpid());
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	ctx.settings.SetString("driver_monadolink", "monado_socket", sock_path.c_str());
	ctx.settings.SetInt32("driver_monadolink", "reconnect_interval_ms", 20);
	FakeXrState *s = fake_runtime();
	FakeMonadoSocket sock(sock_path);

	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	ctx.host.added.at(0).driver->Activate(0);
	REQUIRE(wait_for([&] { return !ctx.host.poses_copy().empty(); }));

	s->lose_instance = true;
	REQUIRE(wait_for([&] { return !ctx.host.poses_copy().back().second.deviceIsConnected; }));
	s->refresh_rate = 120.f; // restarted with other settings
	s->lose_instance = false;
	CHECK(wait_for([&] { return logged(ctx, "Monado's display changed"); }));
	CHECK(wait_for([&] { return ctx.host.poses_copy().back().second.deviceIsConnected; }));

	provider.Cleanup();
}

// --- Runtime selection ---------------------------------------------------------------------------

#include <fstream>

static std::string
write_temp(const char *name, const char *content)
{
	std::string path = std::string(P_tmpdir) + "/monadolink-test-" + name;
	std::ofstream(path) << content;
	return path;
}

TEST_CASE("SteamVR's own OpenXR runtime is refused at once instead of deadlocking vrserver")
{
	fake::DriverContext ctx;
	ctx.install();
	std::string steamxr = write_temp(
	    "steamxr_linux64.json",
	    R"({"file_format_version": "1.0.0", "runtime": {"name": "SteamVR", "library_path": "../bin/linux64/vrclient.so"}})");
	CHECK(ml::is_steamvr_manifest(steamxr));

	ml::XrClient xr;
	const auto t0 = std::chrono::steady_clock::now();
	CHECK_FALSE(xr.start(steamxr.c_str()));
	CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
	bool explained = false;
	for (const auto &l : ctx.log.copy()) {
		explained |= l.find("SteamVR's own runtime") != std::string::npos;
	}
	CHECK(explained);
	remove(steamxr.c_str());
}

TEST_CASE("Monado's and the fake runtime's manifests are accepted")
{
	std::string monado = write_temp(
	    "monado.json", R"({"file_format_version": "1.0.0", "runtime": {"name": "Monado", "library_path": "/x/libopenxr_monado.so"}})");
	CHECK_FALSE(ml::is_steamvr_manifest(monado));
	CHECK_FALSE(ml::is_steamvr_manifest(FAKE_RUNTIME_JSON));
	CHECK_FALSE(ml::is_steamvr_manifest("/nonexistent.json"));
	remove(monado.c_str());
}

TEST_CASE("the manifest path follows XR_RUNTIME_JSON first")
{
	setenv("XR_RUNTIME_JSON", "/some/runtime.json", 1);
	CHECK(ml::runtime_manifest_path() == "/some/runtime.json");
	unsetenv("XR_RUNTIME_JSON");
	std::string dir = std::string(P_tmpdir) + "/monadolink-test-xdg";
	std::string cmd = "mkdir -p " + dir + "/openxr/1";
	REQUIRE(system(cmd.c_str()) == 0);
	write_temp("xdg/openxr/1/active_runtime.json", "{}");
	setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
	CHECK(ml::runtime_manifest_path() == dir + "/openxr/1/active_runtime.json");
	unsetenv("XDG_CONFIG_HOME");
}

TEST_CASE("an implausible IPD from the runtime (0 mm) falls back to 64 mm eyes instead of no stereo")
{
	fake::DriverContext ctx;
	ctx.install();
	FakeXrState *s = fake_runtime();
	s->ipd = 0.f; // Monado once reported this: both eyes at one point

	ml::XrClient xr;
	REQUIRE(xr.start(FAKE_RUNTIME_JSON));
	ml::DisplayConfig c = ml::default_display_config();
	xr.apply_display_info(&c);
	CHECK(c.ipd_meters == doctest::Approx(0.064f));
	REQUIRE(c.has_eye_to_head);
	CHECK(c.eye_to_head[0].m[0][3] == doctest::Approx(-0.032f));
	CHECK(c.eye_to_head[1].m[0][3] == doctest::Approx(0.032f));
	CHECK(xr.eye_pose(1).position.x == doctest::Approx(0.032f));
	bool logged = false;
	for (const auto &l : ctx.log.copy()) {
		logged |= l.find("implausible IPD (0.0 mm)") != std::string::npos;
	}
	CHECK(logged);

	// Still bogus: nothing to update. A real value: the eyes follow.
	XrPosef eyes[2];
	CHECK_FALSE(xr.refresh_eye_poses(eyes));
	s->ipd = 0.068f;
	REQUIRE(xr.refresh_eye_poses(eyes));
	CHECK(eyes[1].position.x == doctest::Approx(0.034f));
	CHECK(xr.eye_pose(0).position.x == doctest::Approx(-0.034f));
	CHECK_FALSE(xr.refresh_eye_poses(eyes)); // unchanged
}

TEST_CASE("an IPD change after Init reaches SteamVR (eye-to-head and the IPD property)")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.settings.SetString("driver_monadolink", "openxr_runtime_json", FAKE_RUNTIME_JSON);
	FakeXrState *s = fake_runtime();
	s->ipd = 0.f;

	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	auto *hmd = ctx.host.added.at(0).driver;
	REQUIRE(hmd->Activate(2) == vr::VRInitError_None);
	const auto c = ctx.properties.TrackedDeviceToPropertyContainer(2);
	CHECK(fake::get_property<float>(ctx.properties, c, vr::Prop_UserIpdMeters_Float) == doctest::Approx(0.064f));

	s->ipd = 0.058f; // the Quest 2's narrowest slider position, now reported
	auto eye_x = [&] {
		std::lock_guard lock(ctx.host.mutex);
		return ctx.host.eye_to_head[1].m[0][3];
	};
	bool updated = false;
	for (int i = 0; i < 60 && !updated; i++) { // checked once a second
		updated = std::fabs(eye_x() - 0.029f) < 1e-5f;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	CHECK(updated);
	CHECK(fake::get_property<float>(ctx.properties, c, vr::Prop_UserIpdMeters_Float) == doctest::Approx(0.058f));
	bool logged = false;
	for (const auto &l : ctx.log.copy()) {
		logged |= l.find("Eye poses changed: IPD 58.0 mm") != std::string::npos;
	}
	CHECK(logged);

	hmd->Deactivate();
	provider.Cleanup();
}

TEST_CASE("the running Monado's manifest (written by start-monado.sh) wins over the setting, also on reconnects")
{
	fake::DriverContext ctx;
	ctx.install();
	const std::string running = std::string(P_tmpdir) + "/monadolink-test-running-" + std::to_string(getpid()) + ".json";
	const std::string sock_path = std::string(P_tmpdir) + "/monadolink-test-monado4-" + std::to_string(getpid());
	unlink(running.c_str());
	ctx.settings.SetString("driver_monadolink", "monado_runtime_file", running.c_str());
	ctx.settings.SetString("driver_monadolink", "monado_socket", sock_path.c_str());
	ctx.settings.SetInt32("driver_monadolink", "reconnect_interval_ms", 20);
	FakeXrState *s = fake_runtime();

	// The setting names nothing usable; the running file doesn't exist yet: not connected.
	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	CHECK(logged(ctx, "Monado not reachable yet"));
	ctx.host.added.at(0).driver->Activate(0);

	// start-monado.sh writes its tree's manifest, Monado listens: the reconnect uses that manifest.
	{
		std::ifstream src(FAKE_RUNTIME_JSON, std::ios::binary);
		std::ofstream(running, std::ios::binary) << src.rdbuf();
	}
	FakeMonadoSocket sock(sock_path);
	CHECK(wait_for([&] { return ctx.host.poses_copy().back().second.deviceIsConnected; }));
	CHECK(logged(ctx, ("runtime manifest " + running).c_str()));
	CHECK(s->instances == 1);

	provider.Cleanup();
	unlink(running.c_str());
}
