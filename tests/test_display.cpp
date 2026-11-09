// SPDX-License-Identifier: MIT
// DisplayComponent geometry, which SteamVR uses to size and place the eye images.
#include "fake_openvr.h"
#include "hmd_device.h"

#include <doctest/doctest.h>

#include <cmath>

static vr::IVRDisplayComponent *
display_of(ml::HmdDevice &hmd)
{
	return static_cast<vr::IVRDisplayComponent *>(hmd.GetComponent(vr::IVRDisplayComponent_Version));
}

TEST_CASE("render size, window and viewports follow the display settings")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::HmdDevice hmd;
	auto *display = display_of(hmd);
	REQUIRE(display != nullptr);

	uint32_t w = 0, h = 0;
	display->GetRecommendedRenderTargetSize(&w, &h);
	CHECK(w == 1832);
	CHECK(h == 1920);

	int32_t wx = -1, wy = -1;
	display->GetWindowBounds(&wx, &wy, &w, &h);
	CHECK(wx == 0);
	CHECK(wy == 0);
	CHECK(w == 2 * 1832); // both eyes side by side
	CHECK(h == 1920);

	uint32_t x = 99, y = 99;
	display->GetEyeOutputViewport(vr::Eye_Left, &x, &y, &w, &h);
	CHECK(x == 0);
	CHECK(w == 1832);
	display->GetEyeOutputViewport(vr::Eye_Right, &x, &y, &w, &h);
	CHECK(x == 1832);
	CHECK(y == 0);
	CHECK(h == 1920);

	CHECK_FALSE(display->IsDisplayOnDesktop());
	CHECK_FALSE(display->IsDisplayRealDisplay());
}

TEST_CASE("render size follows changed settings")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.settings.SetInt32("monadolink_display", "render_width", 1440);
	ctx.settings.SetInt32("monadolink_display", "render_height", 1584);
	ml::HmdDevice hmd;

	uint32_t w = 0, h = 0;
	display_of(hmd)->GetRecommendedRenderTargetSize(&w, &h);
	CHECK(w == 1440);
	CHECK(h == 1584);
}

TEST_CASE("projection is wider outward than inward and mirrored between eyes")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::HmdDevice hmd;
	auto *display = display_of(hmd);

	float l[2], r[2], t[2], b[2];
	display->GetProjectionRaw(vr::Eye_Left, &l[0], &r[0], &t[0], &b[0]);
	display->GetProjectionRaw(vr::Eye_Right, &l[1], &r[1], &t[1], &b[1]);

	for (int eye = 0; eye < 2; eye++) {
		CHECK(l[eye] < 0.f);
		CHECK(r[eye] > 0.f);
		CHECK(t[eye] < 0.f); // OpenVR raw convention: top = tan(down edge), negative
		CHECK(b[eye] > 0.f);
	}
	CHECK(-l[0] > r[0]); // left eye: outward (left) wider than inward
	CHECK(r[1] > -l[1]); // right eye: outward (right) wider
	CHECK(l[0] == doctest::Approx(-r[1]));
	CHECK(r[0] == doctest::Approx(-l[1]));
	CHECK(t[0] == doctest::Approx(t[1]));
	CHECK(b[0] == doctest::Approx(b[1]));
}

TEST_CASE("distortion is the identity (Monado applies the real lens distortion)")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::HmdDevice hmd;
	auto *display = display_of(hmd);

	for (float u : {0.f, 0.25f, 1.f}) {
		for (float v : {0.f, 0.5f, 1.f}) {
			auto d = display->ComputeDistortion(vr::Eye_Right, u, v);
			CHECK(d.rfRed[0] == u);
			CHECK(d.rfGreen[0] == u);
			CHECK(d.rfBlue[0] == u);
			CHECK(d.rfRed[1] == v);
			CHECK(d.rfGreen[1] == v);
			CHECK(d.rfBlue[1] == v);
		}
	}
	vr::HmdVector2_t out{};
	CHECK_FALSE(display->ComputeInverseDistortion(&out, vr::Eye_Left, 0, 0.5f, 0.5f));
}

TEST_CASE("fsr_render_scale shrinks the render size SteamVR is told, keeping it even and clamped")
{
	ml::DisplayConfig c{};
	c.render_width = 1832;
	c.render_height = 1920;
	CHECK(ml::apply_render_scale(&c, 0.75f) == doctest::Approx(0.75f));
	CHECK(c.render_width == 1374);
	CHECK(c.render_height == 1440);
	CHECK(c.fsr_scale == doctest::Approx(0.75f));

	ml::DisplayConfig off{};
	off.render_width = 1001;
	off.render_height = 999;
	for (float scale : {0.f, 1.f, 0.995f, 1.5f}) {
		CHECK(ml::apply_render_scale(&off, scale) == 1.f);
		CHECK(off.render_width == 1001); // untouched, even when odd
		CHECK(off.fsr_scale == 1.f);
	}

	ml::DisplayConfig tiny{};
	tiny.render_width = 1000;
	tiny.render_height = 1000;
	CHECK(ml::apply_render_scale(&tiny, 0.1f) == doctest::Approx(0.5f)); // no lower than half
	CHECK(tiny.render_width == 500);
}

#include "vk/fsr.h"
#include <cstring>

static float
as_float(uint32_t u)
{
	float f;
	memcpy(&f, &u, sizeof(f));
	return f;
}

TEST_CASE("FSR's EASU and RCAS constants come from AMD's setup code")
{
	uint32_t con[16];
	// A 900x960 region of a 1800x1920 image (the game's dynamic resolution), upscaled to 1800x1920.
	ml::fsr_easu_constants(con, 900.f, 960.f, 1800.f, 1920.f, 1800.f, 1920.f);
	CHECK(as_float(con[0]) == doctest::Approx(0.5f));          // output pixel -> region pixel scale
	CHECK(as_float(con[1]) == doctest::Approx(0.5f));
	CHECK(as_float(con[2]) == doctest::Approx(0.5f * 0.5f - 0.5f)); // half-pixel offset
	CHECK(as_float(con[4]) == doctest::Approx(1.f / 1800.f)); // region pixel -> normalized in the image
	CHECK(as_float(con[5]) == doctest::Approx(1.f / 1920.f));

	uint32_t rcas[4];
	ml::fsr_rcas_constants(rcas, 0.f);
	CHECK(as_float(rcas[0]) == doctest::Approx(1.f)); // exp2(-sharpness)
	ml::fsr_rcas_constants(rcas, 1.f);
	CHECK(as_float(rcas[0]) == doctest::Approx(0.5f));
}

#include "util/env_file.h"
#include <unistd.h>

TEST_CASE("read_env_file reads KEY=VALUE lines, skipping comments and trimming quotes")
{
	const std::string path = std::string(P_tmpdir) + "/monadolink-test-env-" + std::to_string(getpid());
	FILE *f = fopen(path.c_str(), "w");
	REQUIRE(f != nullptr);
	fputs("# comment\n\nA=1\n  B = two words \nexport C=\"q\"\n=nokey\nbroken line\nD=\n", f);
	fclose(f);
	auto m = ml::read_env_file(path);
	CHECK(m["A"] == "1");
	CHECK(m["B"] == "two words");
	CHECK(m["C"] == "q");
	CHECK(m.count("D") == 1);
	CHECK(m["D"].empty());
	CHECK(m.size() == 4);
	CHECK(ml::read_env_file("/nonexistent/settings.env").empty());
	CHECK(ml::quest_vr_settings_path("/x.env") == "/x.env");
	remove(path.c_str());
}
