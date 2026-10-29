#include "xr/xr_input.h"

#include "util/log.h"
#include "xr/xr_math.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace ml {

static bool
xr_ok(XrResult r, const char *what)
{
	if (XR_FAILED(r)) {
		log("OpenXR input: %s failed: XrResult %d", what, (int)r);
		return false;
	}
	return true;
}

bool
XrInput::init(XrInstance instance, XrSession session)
{
	instance_ = instance;
	session_ = session;

	XrActionSetCreateInfo sci{XR_TYPE_ACTION_SET_CREATE_INFO};
	snprintf(sci.actionSetName, sizeof(sci.actionSetName), "monadolink");
	snprintf(sci.localizedActionSetName, sizeof(sci.localizedActionSetName), "Monado Link controllers");
	if (!xr_ok(xrCreateActionSet(instance_, &sci, &set_), "xrCreateActionSet")) {
		set_ = XR_NULL_HANDLE;
		return false;
	}
	xrStringToPath(instance_, "/user/hand/left", &hand_path_[k_left]);
	xrStringToPath(instance_, "/user/hand/right", &hand_path_[k_right]);

	struct
	{
		XrAction *action;
		const char *name;
		XrActionType type;
	} actions[] = {
	    {&trigger_, "trigger", XR_ACTION_TYPE_FLOAT_INPUT},
	    {&trigger_touch_, "trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&squeeze_, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT},
	    {&stick_, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT},
	    {&stick_click_, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&stick_touch_, "thumbstick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&thumbrest_touch_, "thumbrest_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&lower_click_, "lower_button_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&lower_touch_, "lower_button_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&upper_click_, "upper_button_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&upper_touch_, "upper_button_touch", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&menu_click_, "menu_click", XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {&grip_pose_, "grip_pose", XR_ACTION_TYPE_POSE_INPUT},
	    {&vibrate_, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT},
	};
	for (auto &a : actions) {
		XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
		snprintf(aci.actionName, sizeof(aci.actionName), "%s", a.name);
		snprintf(aci.localizedActionName, sizeof(aci.localizedActionName), "%s", a.name);
		aci.actionType = a.type;
		aci.countSubactionPaths = 2;
		aci.subactionPaths = hand_path_;
		if (!xr_ok(xrCreateAction(set_, &aci, a.action), a.name)) {
			destroy();
			return false;
		}
	}

	// A suggestion fails as a whole if the runtime doesn't know one path: retry without the ones a
	// runtime may lack (thumbrest, the left menu button).
	if (!suggest_bindings(true) && !suggest_bindings(false)) {
		destroy();
		return false;
	}

	for (int hand = 0; hand < 2; hand++) {
		XrActionSpaceCreateInfo asci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		asci.action = grip_pose_;
		asci.subactionPath = hand_path_[hand];
		asci.poseInActionSpace.orientation.w = 1.f;
		if (!xr_ok(xrCreateActionSpace(session_, &asci, &grip_space_[hand]), "xrCreateActionSpace (grip)")) {
			grip_space_[hand] = XR_NULL_HANDLE;
			destroy();
			return false;
		}
	}

	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &set_;
	if (!xr_ok(xrAttachSessionActionSets(session_, &attach), "xrAttachSessionActionSets")) {
		destroy();
		return false;
	}
	log("OpenXR input: Touch controller actions ready");
	return true;
}

bool
XrInput::suggest_bindings(bool with_optional)
{
	struct Binding
	{
		XrAction action;
		const char *path;
		bool optional;
	};
	const Binding bindings[] = {
	    {trigger_, "/user/hand/left/input/trigger/value", false},
	    {trigger_, "/user/hand/right/input/trigger/value", false},
	    {trigger_touch_, "/user/hand/left/input/trigger/touch", false},
	    {trigger_touch_, "/user/hand/right/input/trigger/touch", false},
	    {squeeze_, "/user/hand/left/input/squeeze/value", false},
	    {squeeze_, "/user/hand/right/input/squeeze/value", false},
	    {stick_, "/user/hand/left/input/thumbstick", false},
	    {stick_, "/user/hand/right/input/thumbstick", false},
	    {stick_click_, "/user/hand/left/input/thumbstick/click", false},
	    {stick_click_, "/user/hand/right/input/thumbstick/click", false},
	    {stick_touch_, "/user/hand/left/input/thumbstick/touch", false},
	    {stick_touch_, "/user/hand/right/input/thumbstick/touch", false},
	    {thumbrest_touch_, "/user/hand/left/input/thumbrest/touch", true},
	    {thumbrest_touch_, "/user/hand/right/input/thumbrest/touch", true},
	    {lower_click_, "/user/hand/left/input/x/click", false},
	    {lower_click_, "/user/hand/right/input/a/click", false},
	    {lower_touch_, "/user/hand/left/input/x/touch", false},
	    {lower_touch_, "/user/hand/right/input/a/touch", false},
	    {upper_click_, "/user/hand/left/input/y/click", false},
	    {upper_click_, "/user/hand/right/input/b/click", false},
	    {upper_touch_, "/user/hand/left/input/y/touch", false},
	    {upper_touch_, "/user/hand/right/input/b/touch", false},
	    {menu_click_, "/user/hand/left/input/menu/click", true},
	    {grip_pose_, "/user/hand/left/input/grip/pose", false},
	    {grip_pose_, "/user/hand/right/input/grip/pose", false},
	    {vibrate_, "/user/hand/left/output/haptic", false},
	    {vibrate_, "/user/hand/right/output/haptic", false},
	};
	std::vector<XrActionSuggestedBinding> suggested;
	for (const auto &b : bindings) {
		if (b.optional && !with_optional) {
			continue;
		}
		XrPath path = XR_NULL_PATH;
		xrStringToPath(instance_, b.path, &path);
		suggested.push_back({b.action, path});
	}
	XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	xrStringToPath(instance_, "/interaction_profiles/oculus/touch_controller", &sb.interactionProfile);
	sb.countSuggestedBindings = (uint32_t)suggested.size();
	sb.suggestedBindings = suggested.data();
	XrResult r = xrSuggestInteractionProfileBindings(instance_, &sb);
	if (XR_FAILED(r)) {
		log("OpenXR input: Touch bindings %s refused (XrResult %d)", with_optional ? "with thumbrest/menu" : "(basic)",
		    (int)r);
		return false;
	}
	if (!with_optional) {
		log("OpenXR input: bound without thumbrest/menu");
	}
	return true;
}

void
XrInput::destroy()
{
	for (auto &s : grip_space_) {
		if (s != XR_NULL_HANDLE) {
			xrDestroySpace(s);
			s = XR_NULL_HANDLE;
		}
	}
	if (set_ != XR_NULL_HANDLE) {
		xrDestroyActionSet(set_); // destroys its actions too
		set_ = XR_NULL_HANDLE;
	}
}

bool
XrInput::sync(XrTime time, XrSpace base, double world_offset_y, const XrPosef offset[2], HandState out[2])
{
	for (int hand = 0; hand < 2; hand++) {
		out[hand] = {};
	}
	if (set_ == XR_NULL_HANDLE) {
		return false;
	}
	XrActiveActionSet active{set_, XR_NULL_PATH};
	XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
	si.countActiveActionSets = 1;
	si.activeActionSets = &active;
	XrResult r = xrSyncActions(session_, &si);
	if (XR_FAILED(r)) {
		if (sync_failures_++ % 1000 == 0) {
			log("OpenXR input: xrSyncActions failed: XrResult %d", (int)r);
		}
		return false;
	}
	if (r == XR_SESSION_NOT_FOCUSED) {
		return true; // a success code: all inputs inactive until Monado focuses us
	}

	for (int hand = 0; hand < 2; hand++) {
		HandState &h = out[hand];
		XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
		gi.subactionPath = hand_path_[hand];

		auto boolean = [&](XrAction a) {
			XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
			gi.action = a;
			return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &gi, &s)) && s.isActive && s.currentState;
		};
		auto scalar = [&](XrAction a) {
			XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
			gi.action = a;
			return XR_SUCCEEDED(xrGetActionStateFloat(session_, &gi, &s)) && s.isActive ? s.currentState : 0.f;
		};
		h.trigger = scalar(trigger_);
		h.squeeze = scalar(squeeze_);
		h.trigger_touch = boolean(trigger_touch_);
		h.stick_click = boolean(stick_click_);
		h.stick_touch = boolean(stick_touch_);
		h.thumbrest_touch = boolean(thumbrest_touch_);
		h.lower_click = boolean(lower_click_);
		h.lower_touch = boolean(lower_touch_);
		h.upper_click = boolean(upper_click_);
		h.upper_touch = boolean(upper_touch_);
		h.menu_click = hand == k_left && boolean(menu_click_);
		XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
		gi.action = stick_;
		if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &gi, &v)) && v.isActive) {
			h.stick_x = v.currentState.x;
			h.stick_y = v.currentState.y;
		}

		XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
		gi.action = grip_pose_;
		if (XR_FAILED(xrGetActionStatePose(session_, &gi, &ps)) || !ps.isActive) {
			continue;
		}
		XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
		XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION, &vel};
		if (XR_FAILED(xrLocateSpace(grip_space_[hand], base, time, &loc)) ||
		    (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
			continue;
		}
		// The offset is in the grip's own frame: compose after locating. (Velocities stay the grip's:
		// the difference at a ~10 cm offset is small and SteamVR only extrapolates with them.)
		loc.pose = pose_compose(loc.pose, offset[hand]);
		h.pose = driver_pose_from_xr(loc, vel, world_offset_y);
		h.active = true;
	}
	return true;
}

void
XrInput::haptic(Hand hand, float duration_s, float frequency, float amplitude)
{
	if (set_ == XR_NULL_HANDLE) {
		return;
	}
	XrHapticVibration vib{XR_TYPE_HAPTIC_VIBRATION};
	vib.duration = duration_s > 0.f ? (XrDuration)(duration_s * 1e9) : XR_MIN_HAPTIC_DURATION;
	vib.frequency = frequency > 0.f ? frequency : XR_FREQUENCY_UNSPECIFIED;
	vib.amplitude = amplitude < 0.f ? 0.f : amplitude > 1.f ? 1.f : amplitude;
	XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
	hi.action = vibrate_;
	hi.subactionPath = hand_path_[hand];
	xrApplyHapticFeedback(session_, &hi, (const XrHapticBaseHeader *)&vib);
}

} // namespace ml
