// Shared between fake_openxr_runtime.so and the tests: what the fake runtime reports, and what it saw.
// The tests dlopen the runtime themselves (so it stays loaded across instances) and reach this
// struct with dlsym("fake_xr_state").
#pragma once

#include <openxr/openxr.h>

#include <cstdint>

struct FakeXrState
{
	// Configuration
	bool support_headless = true;
	bool support_stage = true;
	bool support_refresh_rate = true;
	uint32_t width[2] = {1440, 1440}, height[2] = {1584, 1584}; //!< recommended per eye
	float refresh_rate = 90.f;
	XrFovf fov[2] = {{-0.90f, 0.75f, 0.80f, -0.85f}, {-0.75f, 0.90f, 0.80f, -0.85f}}; //!< l, r, u, d
	float ipd = 0.064f;
	XrPosef head = {{0.f, 0.f, 0.f, 1.f}, {0.f, 1.7f, 0.f}};
	XrSpaceLocationFlags head_flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
	                                  XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
	                                  XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	XrVector3f linear_velocity = {0.f, 0.f, 0.f};
	XrVector3f angular_velocity = {0.f, 0.f, 0.f};
	bool lose_instance = false; //!< next xrPollEvent: INSTANCE_LOSS_PENDING, then calls fail with INSTANCE_LOST

	// Controllers (indexed by hand: 0 = left, 1 = right)
	struct Hand
	{
		bool active = true; //!< bound: its actions are active
		XrPosef grip = {{0.f, 0.f, 0.f, 1.f}, {0.f, 1.2f, -0.3f}};
		float trigger = 0.f, squeeze = 0.f, stick_x = 0.f, stick_y = 0.f;
		bool trigger_touch = false, stick_click = false, stick_touch = false, thumbrest_touch = false;
		bool lower_click = false, lower_touch = false, upper_click = false, upper_touch = false, menu_click = false;
	};
	Hand hands[2];
	bool focused = true;                  //!< xrSyncActions returns XR_SESSION_NOT_FOCUSED when false
	bool reject_optional_bindings = false; //!< refuse suggestions containing thumbrest/menu paths

	// Observations
	int instances = 0, sessions = 0, spaces = 0; //!< alive
	bool session_begun = false;
	bool session_had_graphics_binding = false;
	XrReferenceSpaceType base_type = XR_REFERENCE_SPACE_TYPE_MAX_ENUM; //!< base of the last xrLocateSpace
	XrTime last_locate_time = 0;
	int locate_calls = 0;
	int action_sets = 0, actions = 0, suggest_calls = 0, suggested_bindings = 0, sync_calls = 0;
	bool attached = false;
	int haptic_calls = 0, haptic_hand = -1;
	float haptic_amplitude = 0.f, haptic_frequency = 0.f;
	int64_t haptic_duration = 0;
};
