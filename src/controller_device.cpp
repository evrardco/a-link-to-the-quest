// SPDX-License-Identifier: MIT
#include "controller_device.h"

#include "util/log.h"

#include <cstring>

namespace ml {

//! Analog values above this count as a click (Touch triggers and grips have no click switch).
static constexpr float k_click_threshold = 0.7f;
//! Grip "touch": Touch controllers have no grip sensor, so a light squeeze stands in for it.
static constexpr float k_grip_touch_threshold = 0.1f;

static vr::DriverPose_t
disconnected_pose()
{
	vr::DriverPose_t p{};
	p.qWorldFromDriverRotation.w = 1.0;
	p.qDriverFromHeadRotation.w = 1.0;
	p.qRotation.w = 1.0;
	p.poseIsValid = false;
	p.deviceIsConnected = false;
	p.result = vr::TrackingResult_Uninitialized;
	return p;
}

ControllerDevice::ControllerDevice(Hand hand, std::string serial, std::string input_profile)
    : hand_(hand), serial_(std::move(serial)), input_profile_(std::move(input_profile)), pose_(disconnected_pose())
{}

vr::EVRInitError
ControllerDevice::Activate(uint32_t object_id)
{
	device_index_ = object_id;
	const bool left = hand_ == k_left;
	vr::PropertyContainerHandle_t c = vr::VRProperties()->TrackedDeviceToPropertyContainer(object_id);

	// Like ALVR's Quest 2 Touch emulation: "oculus_touch" makes SteamVR and games use their Oculus
	// Touch bindings, render model and icons.
	vr::VRProperties()->SetInt32Property(c, vr::Prop_ControllerRoleHint_Int32,
	                                     left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
	vr::VRProperties()->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_Controller);
	vr::VRProperties()->SetStringProperty(c, vr::Prop_ControllerType_String, "oculus_touch");
	vr::VRProperties()->SetStringProperty(c, vr::Prop_InputProfilePath_String, input_profile_.c_str());
	vr::VRProperties()->SetStringProperty(c, vr::Prop_ManufacturerName_String, "Oculus");
	vr::VRProperties()->SetStringProperty(c, vr::Prop_ModelNumber_String,
	                                      left ? "Miramar (Left Controller)" : "Miramar (Right Controller)");
	vr::VRProperties()->SetStringProperty(c, vr::Prop_RenderModelName_String,
	                                      left ? "oculus_quest2_controller_left" : "oculus_quest2_controller_right");
	vr::VRProperties()->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "monadolink"); // same as the HMD
	vr::VRProperties()->SetStringProperty(c, vr::Prop_SerialNumber_String, serial_.c_str());
	vr::VRProperties()->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
	vr::VRProperties()->SetBoolProperty(c, vr::Prop_DeviceCanPowerOff_Bool, false);

	// Input paths of SteamVR's Oculus Touch profile (the ones ALVR creates for oculus_touch).
	struct Def
	{
		Index index;
		const char *path;
		enum
		{
			Bool,
			OneSided,
			TwoSided
		} kind;
	};
	const Def common[] = {
	    {TriggerValue, "/input/trigger/value", Def::OneSided}, {TriggerClick, "/input/trigger/click", Def::Bool},
	    {TriggerTouch, "/input/trigger/touch", Def::Bool},     {GripValue, "/input/grip/value", Def::OneSided},
	    {GripClick, "/input/grip/click", Def::Bool},           {GripTouch, "/input/grip/touch", Def::Bool},
	    {StickX, "/input/joystick/x", Def::TwoSided},          {StickY, "/input/joystick/y", Def::TwoSided},
	    {StickClick, "/input/joystick/click", Def::Bool},      {StickTouch, "/input/joystick/touch", Def::Bool},
	    {ThumbrestTouch, "/input/thumbrest/touch", Def::Bool},
	};
	const Def left_only[] = {
	    {LowerClick, "/input/x/click", Def::Bool}, {LowerTouch, "/input/x/touch", Def::Bool},
	    {UpperClick, "/input/y/click", Def::Bool}, {UpperTouch, "/input/y/touch", Def::Bool},
	    {MenuClick, "/input/system/click", Def::Bool}, // the left menu button, as SteamVR's Oculus profile has it
	};
	const Def right_only[] = {
	    {LowerClick, "/input/a/click", Def::Bool}, {LowerTouch, "/input/a/touch", Def::Bool},
	    {UpperClick, "/input/b/click", Def::Bool}, {UpperTouch, "/input/b/touch", Def::Bool},
	};
	auto create = [&](const Def &d) {
		vr::VRInputComponentHandle_t *h = &components_[d.index].handle;
		if (d.kind == Def::Bool) {
			vr::VRDriverInput()->CreateBooleanComponent(c, d.path, h);
		} else {
			vr::VRDriverInput()->CreateScalarComponent(
			    c, d.path, h, vr::VRScalarType_Absolute,
			    d.kind == Def::OneSided ? vr::VRScalarUnits_NormalizedOneSided : vr::VRScalarUnits_NormalizedTwoSided);
		}
	};
	for (const auto &d : common) {
		create(d);
	}
	if (left) {
		for (const auto &d : left_only) {
			create(d);
		}
	} else {
		for (const auto &d : right_only) {
			create(d);
		}
	}
	vr::VRDriverInput()->CreateHapticComponent(c, "/output/haptic", &haptic_);

	activated_ = true;
	log("%s controller activated as device %u (%s)", left ? "Left" : "Right", object_id, process_tag());
	return vr::VRInitError_None;
}

void
ControllerDevice::Deactivate()
{
	activated_ = false;
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}

void
ControllerDevice::DebugRequest(const char *request, char *response, uint32_t response_size)
{
	if (response_size > 0) {
		response[0] = '\0';
	}
}

vr::DriverPose_t
ControllerDevice::GetPose()
{
	std::lock_guard lock(pose_mutex_);
	return pose_;
}

void
ControllerDevice::set(Index i, float value)
{
	Component &comp = components_[i];
	if (comp.handle == vr::k_ulInvalidInputComponentHandle || comp.last == value) {
		return;
	}
	comp.last = value;
	if (i == TriggerValue || i == GripValue || i == StickX || i == StickY) {
		vr::VRDriverInput()->UpdateScalarComponent(comp.handle, value, 0.0);
	} else {
		vr::VRDriverInput()->UpdateBooleanComponent(comp.handle, value != 0.f, 0.0);
	}
}

void
ControllerDevice::update(const HandState &s)
{
	vr::DriverPose_t pose = s.active ? s.pose : disconnected_pose();
	if (!s.active) {
		// Keep the last position, so the controller doesn't jump to the origin when tracking drops.
		std::lock_guard lock(pose_mutex_);
		pose = pose_;
		pose.poseIsValid = false;
		pose.result = vr::TrackingResult_Running_OutOfRange;
	}
	{
		std::lock_guard lock(pose_mutex_);
		pose_ = pose;
	}
	const uint32_t index = device_index_;
	if (index == vr::k_unTrackedDeviceIndexInvalid || !activated_) {
		return;
	}
	vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index, pose, sizeof(pose));

	set(TriggerValue, s.trigger);
	set(TriggerClick, s.trigger > k_click_threshold);
	set(TriggerTouch, s.trigger_touch || s.trigger > 0.f);
	set(GripValue, s.squeeze);
	set(GripClick, s.squeeze > k_click_threshold);
	set(GripTouch, s.squeeze > k_grip_touch_threshold);
	set(StickX, s.stick_x);
	set(StickY, s.stick_y);
	set(StickClick, s.stick_click);
	set(StickTouch, s.stick_touch || s.stick_click);
	set(ThumbrestTouch, s.thumbrest_touch);
	set(LowerClick, s.lower_click);
	set(LowerTouch, s.lower_touch || s.lower_click);
	set(UpperClick, s.upper_click);
	set(UpperTouch, s.upper_touch || s.upper_click);
	set(MenuClick, s.menu_click);
}

} // namespace ml
