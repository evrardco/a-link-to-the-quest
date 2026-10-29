// IServerTrackedDeviceProvider: owns the devices and is ticked by vrserver every frame.
#pragma once

#include "controller_device.h"
#include "hmd_device.h"
#include "offset_tuner.h"
#include "xr/xr_client.h"

#include <openvr_driver.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace ml {

class DeviceProvider : public vr::IServerTrackedDeviceProvider
{
public:
	vr::EVRInitError
	Init(vr::IVRDriverContext *driver_context) override;
	void
	Cleanup() override;
	const char *const *
	GetInterfaceVersions() override
	{
		return vr::k_InterfaceVersions;
	}
	void
	RunFrame() override;
	bool
	ShouldBlockStandbyMode() override
	{
		return false;
	}
	void
	EnterStandby() override
	{}
	void
	LeaveStandby() override
	{}

	//! Re-read the controller_offset_* settings; true (and logged) if they changed. RunFrame calls it
	//! twice a second, so tools/tune (or anything setting them through IVRSettings) works live.
	bool
	reload_controller_offsets();

private:
	void
	tracking_loop();
	//! Tracking thread: in-headset tuning (offset_tuner.h); may hide the hands' buttons from the game.
	void
	run_tuner(HandState hands[2]);
	//! RunFrame: write values tuned in the headset to the settings (SteamVR persists them); true if
	//! the defaults were restored (the caller reloads them).
	bool
	save_tuned_offsets();

	std::unique_ptr<XrClient> xr_; //!< null when Monado wasn't reachable at Init
	std::unique_ptr<HmdDevice> hmd_;
	std::unique_ptr<ControllerDevice> controllers_[2]; //!< null without Monado's controller input
	std::mutex offset_mutex_;                          //!< guards the two below (tracking thread reads)
	XrPosef controller_offset_[2];                     //!< grip -> SteamVR controller pose (settings)
	float offset_settings_[6] = {};                    //!< x, y, z (m), pitch, yaw, roll (deg) last read
	bool offsets_loaded_ = false;
	bool tuned_dirty_ = false;     //!< offset_settings_ tuned in the headset, not yet in the settings
	bool reset_requested_ = false; //!< tuner asked for the defaults
	OffsetTuner tuner_;            //!< tracking thread only
	bool tuning_enabled_ = false;
	std::chrono::steady_clock::time_point next_offset_check_{}; //!< RunFrame only
	std::thread tracking_thread_;
	std::atomic<bool> stop_tracking_{false};
};

} // namespace ml
