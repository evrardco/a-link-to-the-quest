// SPDX-License-Identifier: MIT
// DeviceProvider + HmdDevice against fake SteamVR host interfaces.
#include "device_provider.h"
#include "fake_openvr.h"

#include <doctest/doctest.h>

#include <cstring>

TEST_CASE("Init adds the HMD with the configured serial first, then the controllers")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DeviceProvider provider;

	// No Monado here: the controllers are added anyway (inactive until it connects).
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	REQUIRE(ctx.host.added.size() == 3);
	CHECK(ctx.host.added[0].serial == "MONADOLINK-0001");
	CHECK(ctx.host.added[0].device_class == vr::TrackedDeviceClass_HMD);
	CHECK(ctx.host.added[1].device_class == vr::TrackedDeviceClass_Controller);
	provider.Cleanup();
}

TEST_CASE("Init reports an error when SteamVR refuses the device")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.host.accept_devices = false;
	ml::DeviceProvider provider;

	CHECK(provider.Init(&ctx) != vr::VRInitError_None);
	provider.Cleanup();
}

TEST_CASE("activated HMD advertises direct mode, its display and 120 Hz")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);
	auto *hmd = ctx.host.added.at(0).driver;

	REQUIRE(hmd->Activate(0) == vr::VRInitError_None);
	const auto c = ctx.properties.TrackedDeviceToPropertyContainer(0);
	CHECK(fake::get_property<bool>(ctx.properties, c, vr::Prop_HasDriverDirectModeComponent_Bool));
	CHECK(fake::get_property<float>(ctx.properties, c, vr::Prop_DisplayFrequency_Float) == doctest::Approx(120.f));
	CHECK(fake::get_property<float>(ctx.properties, c, vr::Prop_UserIpdMeters_Float) == doctest::Approx(0.063f));
	CHECK_FALSE(fake::get_property<bool>(ctx.properties, c, vr::Prop_IsOnDesktop_Bool));
	CHECK(fake::get_string_property(ctx.properties, c, vr::Prop_ModelNumber_String) == "Monado Link");
	CHECK(fake::get_string_property(ctx.properties, c, vr::Prop_InputProfilePath_String) ==
	      "{monadolink}/input/monadolink_hmd_profile.json");

	CHECK(hmd->GetComponent(vr::IVRDisplayComponent_Version) != nullptr);
	CHECK(hmd->GetComponent(vr::IVRDriverDirectModeComponent_Version) != nullptr);
	CHECK(hmd->GetComponent("IVRCameraComponent_003") == nullptr);
	CHECK(hmd->GetComponent("IVRDisplayComponent_002") == nullptr); // only the exact version

	hmd->Deactivate();
	provider.Cleanup();
}

TEST_CASE("without Monado, RunFrame publishes a not-tracking pose once the HMD is active")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DeviceProvider provider;
	REQUIRE(provider.Init(&ctx) == vr::VRInitError_None);

	provider.RunFrame();
	CHECK(ctx.host.poses_copy().empty()); // not activated yet: nothing to update

	ctx.host.added.at(0).driver->Activate(3);
	provider.RunFrame();
	const auto poses = ctx.host.poses_copy();
	REQUIRE_FALSE(poses.empty());
	const auto &[device, pose] = poses.back();
	CHECK(device == 3);
	CHECK_FALSE(pose.poseIsValid);
	CHECK_FALSE(pose.deviceIsConnected);
	CHECK(pose.qRotation.w == doctest::Approx(1.0));

	provider.Cleanup();
}
