// SPDX-License-Identifier: MIT
// StderrCapture: stderr output (the OpenXR loader's and Monado's errors) lands in SteamVR's log.
#include "device_provider.h"
#include "fake_openvr.h"
#include "util/stderr_capture.h"

#include <doctest/doctest.h>

#include <cstdio>

TEST_CASE("stderr lines are forwarded to the driver log while capturing, and only then")
{
	fake::DriverContext ctx;
	ctx.install();
	{
		ml::StderrCapture capture("X: ");
		fprintf(stderr, "first line\nsecond ");
		fprintf(stderr, "line\nunterminated");
	}
	fprintf(stderr, "after the capture\n");

	std::vector<std::string> got;
	for (const auto &l : ctx.log.copy()) {
		if (l.find("X: ") != std::string::npos) {
			got.push_back(l);
		}
	}
	REQUIRE(got.size() == 3);
	CHECK(got[0] == "[monadolink] X: first line");
	CHECK(got[1] == "[monadolink] X: second line");
	CHECK(got[2] == "[monadolink] X: unterminated");
	for (const auto &l : ctx.log.copy()) {
		CHECK(l.find("after the capture") == std::string::npos);
	}
}

TEST_CASE("a failed connection's loader errors reach the log through DeviceProvider::Init")
{
	fake::DriverContext ctx;
	ctx.install(); // the runtime manifest path doesn't exist: the loader complains on stderr
	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	bool found = false;
	for (const auto &l : ctx.log.copy()) {
		found |= l.find("OpenXR stderr: ") != std::string::npos && l.find("nonexistent") != std::string::npos;
	}
	CHECK(found);
	provider.Cleanup();
}
