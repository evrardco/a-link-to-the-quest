// A Touch controller as SteamVR sees it (M4): an "oculus_touch" controller with the Quest 2 render
// model, so games' existing Oculus Touch bindings apply. Fed from XrInput by the tracking thread.
#pragma once

#include "xr/xr_input.h"

#include <openvr_driver.h>

#include <atomic>
#include <mutex>
#include <string>

namespace ml {

class ControllerDevice : public vr::ITrackedDeviceServerDriver
{
public:
	ControllerDevice(Hand hand, std::string serial, std::string input_profile);

	vr::EVRInitError
	Activate(uint32_t object_id) override;
	void
	Deactivate() override;
	void
	EnterStandby() override
	{}
	void *
	GetComponent(const char *component_name_and_version) override
	{
		return nullptr;
	}
	void
	DebugRequest(const char *request, char *response, uint32_t response_size) override;
	vr::DriverPose_t
	GetPose() override;

	//! New state from XrInput (tracking thread): pose and the input components that changed.
	void
	update(const HandState &s);
	//! Whether SteamVR's haptic event is for this controller.
	bool
	owns_haptic(vr::VRInputComponentHandle_t component) const
	{
		return activated_ && component == haptic_ && component != vr::k_ulInvalidInputComponentHandle;
	}
	Hand
	hand() const
	{
		return hand_;
	}
	const char *
	serial() const
	{
		return serial_.c_str();
	}

private:
	//! One input component and the value last sent (to send changes only).
	struct Component
	{
		vr::VRInputComponentHandle_t handle = vr::k_ulInvalidInputComponentHandle;
		float last = -2.f; //!< never a real value: the first update always goes out
	};
	enum Index
	{
		TriggerValue,
		TriggerClick,
		TriggerTouch,
		GripValue,
		GripClick,
		GripTouch,
		StickX,
		StickY,
		StickClick,
		StickTouch,
		ThumbrestTouch,
		LowerClick,
		LowerTouch,
		UpperClick,
		UpperTouch,
		MenuClick, //!< left only
		k_component_count
	};
	void
	set(Index i, float value);

	Hand hand_;
	std::string serial_;
	std::string input_profile_;
	std::atomic<uint32_t> device_index_{vr::k_unTrackedDeviceIndexInvalid};
	std::atomic<bool> activated_{false}; //!< components exist: update() may use them
	std::mutex pose_mutex_;
	vr::DriverPose_t pose_;
	Component components_[k_component_count];
	vr::VRInputComponentHandle_t haptic_ = vr::k_ulInvalidInputComponentHandle;
};

} // namespace ml
