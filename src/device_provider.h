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
#include <string>
#include <thread>

namespace ml {

//! Monado's IPC socket: `setting` (driver_monadolink/monado_socket) if set, else
//! $XDG_RUNTIME_DIR/monado_comp_ipc (/tmp without XDG_RUNTIME_DIR, like Monado).
std::string
monado_socket_path(const std::string &setting);
//! Whether a process listens on the unix socket `path` (/proc/net/unix; file exists as a fallback).
bool
unix_socket_listening(const std::string &path);

class DeviceProvider : public vr::IServerTrackedDeviceProvider
{
public:
	~DeviceProvider()
	{
		stop_tracking_ = true; // a Cleanup() that never came must not leave the thread running
		if (tracking_thread_.joinable()) {
			tracking_thread_.join();
		}
	}
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
	//! Tracking thread (tuner's B): touch the file Monado watches, so it sends a fresh keyframe.
	void
	request_stream_reset();
	//! The OpenXR runtime manifest to use: the running Monado's (monado_runtime_file_path), else the
	//! openxr_runtime_json setting.
	std::string
	runtime_json();
	//! Tracking thread (tuner's X): floor correction from the lower tracked controller, lying on the
	//! floor; refused (a long buzz) if it would move the floor by more than 1 m.
	void
	set_floor_from_controllers(const HandState hands[2]);
	//! Tracking thread, Monado lost and its socket back: restart the OpenXR client (logged).
	bool
	reconnect();

	std::unique_ptr<XrClient> xr_; //!< lost (and retried) when Monado wasn't reachable at Init
	DisplayConfig display_;        //!< what SteamVR was told at Init
	std::string monado_socket_;
	std::chrono::milliseconds reconnect_interval_{2000}; //!< setting reconnect_interval_ms
	//! Monado wasn't reachable at Init: the HMD has the placeholder display until the first connection.
	bool display_pending_ = false;
	float fsr_scale_ = 1.f; //!< the FSR render scale applied at Init
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
	std::string stream_reset_path_; //!< the file Monado watches (settings, read at Init)
	std::chrono::steady_clock::time_point next_offset_check_{}; //!< RunFrame only
	std::thread tracking_thread_;
	std::atomic<bool> stop_tracking_{false};
};

} // namespace ml
