// SPDX-License-Identifier: MIT
// The HMD: a tracked device with a display component and a driver direct-mode component.
#pragma once

#include "direct/direct_mode.h"
#include "display_config.h"

#include <openvr_driver.h>

#include <atomic>
#include <mutex>
#include <string>

namespace ml {

class XrClient;

//! The display from the settings (resolution, refresh) and a placeholder Quest 2 FOV; Monado's values
//! replace these when it's reachable.
DisplayConfig
default_display_config();

class DisplayComponent : public vr::IVRDisplayComponent
{
public:
	explicit DisplayComponent(const DisplayConfig &config) : config_(config) {}
	//! Monado's display arrived after SteamVR started (HmdDevice::update_display).
	void
	set(const DisplayConfig &config)
	{
		std::lock_guard lock(mutex_);
		config_ = config;
	}

	void
	GetWindowBounds(int32_t *x, int32_t *y, uint32_t *width, uint32_t *height) override;
	bool
	IsDisplayOnDesktop() override
	{
		return false;
	}
	bool
	IsDisplayRealDisplay() override
	{
		return false;
	}
	void
	GetRecommendedRenderTargetSize(uint32_t *width, uint32_t *height) override;
	void
	GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *width, uint32_t *height) override;
	void
	GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top, float *bottom) override;
	vr::DistortionCoordinates_t
	ComputeDistortion(vr::EVREye eye, float u, float v) override;
	bool
	ComputeInverseDistortion(vr::HmdVector2_t *result, vr::EVREye eye, uint32_t channel, float u, float v) override
	{
		return false;
	}

private:
	std::mutex mutex_; //!< set() from the tracking thread, the getters from SteamVR's
	DisplayConfig config_;
};

class HmdDevice : public vr::ITrackedDeviceServerDriver
{
public:
	//! `xr` (may be null) is where frames go; it must outlive the device.
	explicit HmdDevice(const DisplayConfig &display, XrClient *xr = nullptr);
	HmdDevice() : HmdDevice(default_display_config()) {}
	//! Minimal logging for the frame path (DirectModeComponent::set_quiet). Call before Activate.
	void
	set_quiet_logs(bool quiet)
	{
		direct_mode_.set_quiet(quiet);
	}

	vr::EVRInitError
	Activate(uint32_t object_id) override;
	void
	Deactivate() override;
	void
	EnterStandby() override
	{}
	void *
	GetComponent(const char *component_name_and_version) override;
	void
	DebugRequest(const char *request, char *response, uint32_t response_size) override;
	vr::DriverPose_t
	GetPose() override;

	const char *
	serial() const
	{
		return serial_.c_str();
	}
	//! Store the latest pose (returned by GetPose) and publish it to SteamVR once activated.
	//! Thread-safe: the tracking thread calls it.
	void
	update_pose(const vr::DriverPose_t &pose);
	//! New eye-in-head poses (IPD change): tells SteamVR (SetDisplayEyeToHead, Prop_UserIpdMeters).
	void
	update_eyes(const XrPosef eyes[2]);
	//! A copy of the display SteamVR was given.
	DisplayConfig
	display_config()
	{
		std::lock_guard lock(eyes_mutex_);
		return display_config_;
	}
	//! Monado connected after SteamVR started (the HMD has the placeholder display): give SteamVR
	//! Monado's projection, render size, refresh rate and eye poses, and from now on drop frames
	//! rendered with another projection (in case SteamVR keeps the old one). Tracking thread.
	void
	update_display(const DisplayConfig &display);
	//! Without Monado: republish the static placeholder pose (from RunFrame).
	void
	run_frame();

private:
	std::atomic<uint32_t> device_index_{vr::k_unTrackedDeviceIndexInvalid};
	std::mutex pose_mutex_;
	vr::DriverPose_t pose_;
	std::string serial_;
	std::string model_;
	DisplayConfig display_config_;
	std::mutex eyes_mutex_; //!< guards display_config_'s ipd_meters/eye_to_head (update_eyes vs Activate)
	DisplayComponent display_;
	DirectModeComponent direct_mode_;
};

} // namespace ml
