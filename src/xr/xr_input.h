// SPDX-License-Identifier: MIT
// Touch controller input through OpenXR actions (M4): one action set bound to
// /interaction_profiles/oculus/touch_controller, with the hands as subaction paths (so one "lower
// button" action is A on the right and X on the left), a grip space per hand, and haptics.
//
// Input only flows while the session is FOCUSED (Monado focuses our Vulkan session once it shows
// frames); until then the hands are inactive.
#pragma once

#include <openvr_driver.h>
#include <openxr/openxr.h>

namespace ml {

enum Hand
{
	k_left = 0,
	k_right = 1
};

//! Everything the OpenVR side needs about one controller, from one xrSyncActions.
struct HandState
{
	bool active;           //!< bound, and its pose is tracked
	vr::DriverPose_t pose; //!< grip pose + offset, in SteamVR's universe
	float trigger, squeeze, stick_x, stick_y;
	bool trigger_touch, stick_click, stick_touch, thumbrest_touch;
	bool lower_click, lower_touch; //!< A (right) / X (left)
	bool upper_click, upper_touch; //!< B (right) / Y (left)
	bool menu_click;               //!< left menu button (the right one is the Quest's system button)
};

class XrInput
{
public:
	//! Actions, suggested bindings, grip spaces, attach. False (logged) if the runtime refuses.
	bool
	init(XrInstance instance, XrSession session);
	void
	destroy();
	bool
	ready() const
	{
		return set_ != XR_NULL_HANDLE;
	}
	//! xrSyncActions, then every hand's state at `time`; poses get `offset[hand]` (in the grip's own
	//! frame) and are located in `base`, lifted by `world_offset_y`.
	bool
	sync(XrTime time, XrSpace base, double world_offset_y, const XrPosef offset[2], HandState out[2]);
	//! Vibrate a hand; duration 0 = the runtime's shortest pulse, frequency 0 = its default.
	void
	haptic(Hand hand, float duration_s, float frequency, float amplitude);

private:
	bool
	suggest_bindings(bool with_optional);

	XrInstance instance_ = XR_NULL_HANDLE;
	XrSession session_ = XR_NULL_HANDLE;
	XrActionSet set_ = XR_NULL_HANDLE;
	XrPath hand_path_[2] = {XR_NULL_PATH, XR_NULL_PATH};
	XrAction trigger_ = XR_NULL_HANDLE, trigger_touch_ = XR_NULL_HANDLE, squeeze_ = XR_NULL_HANDLE;
	XrAction stick_ = XR_NULL_HANDLE, stick_click_ = XR_NULL_HANDLE, stick_touch_ = XR_NULL_HANDLE;
	XrAction thumbrest_touch_ = XR_NULL_HANDLE;
	XrAction lower_click_ = XR_NULL_HANDLE, lower_touch_ = XR_NULL_HANDLE;
	XrAction upper_click_ = XR_NULL_HANDLE, upper_touch_ = XR_NULL_HANDLE;
	XrAction menu_click_ = XR_NULL_HANDLE;
	XrAction grip_pose_ = XR_NULL_HANDLE, vibrate_ = XR_NULL_HANDLE;
	XrSpace grip_space_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	uint64_t sync_failures_ = 0;
};

} // namespace ml
