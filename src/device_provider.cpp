#include "device_provider.h"

#include "util/log.h"
#include "util/stderr_capture.h"
#include "xr/xr_math.h"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

namespace ml {

//! How often the tracking thread samples the head. SteamVR extrapolates between samples from the
//! velocities, so this only bounds how stale a pose can be.
static constexpr std::chrono::milliseconds k_tracking_interval{2};

static std::string
get_setting_string(const char *section, const char *key)
{
	char buf[1024] = {};
	vr::VRSettings()->GetString(section, key, buf, sizeof(buf));
	return buf;
}

vr::EVRInitError
DeviceProvider::Init(vr::IVRDriverContext *driver_context)
{
	VR_INIT_SERVER_DRIVER_CONTEXT(driver_context);
	log("Init (%s)", process_tag());

	// SteamVR runs vrserver inside the Steam Linux Runtime container, which doesn't show the host's
	// whole XDG_RUNTIME_DIR: Monado's client then waits ~20 s for its socket and fails. Say so.
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	std::string socket = std::string(runtime_dir != nullptr ? runtime_dir : "/tmp") + "/monado_comp_ipc";
	if (access(socket.c_str(), F_OK) == 0) {
		log("Monado socket %s is visible", socket.c_str());
	} else {
		log("Monado socket %s is NOT visible here (Monado not running, or SteamVR's container hides it: "
		    "set SteamVR's launch options to PRESSURE_VESSEL_FILESYSTEMS_RW=$XDG_RUNTIME_DIR/monado_comp_ipc "
		    "%%command%%)",
		    socket.c_str());
	}

	// Connect to Monado before adding the HMD, so it reports Monado's resolution, FOV and refresh.
	DisplayConfig display = default_display_config();
	xr_ = std::make_unique<XrClient>();
	bool connected;
	{
		// The loader and Monado's client explain their failures only on stderr.
		StderrCapture capture("OpenXR stderr: ");
		// submit_frames off = M1 behaviour (headless session, tracking only): a way out if the frame
		// path (Vulkan, new GPU code) misbehaves.
		connected = xr_->start(get_setting_string("driver_monadolink", "openxr_runtime_json").c_str(),
		                       vr::VRSettings()->GetBool("driver_monadolink", "submit_frames"));
		if (connected) {
			xr_->apply_display_info(&display);
		}
	}
	if (!connected) {
		log("Monado not reachable: default display and a static head pose (start Monado and accept Quest "
		    "Link before SteamVR)");
		xr_.reset();
	}

	hmd_ = std::make_unique<HmdDevice>(display, xr_.get());
	if (!vr::VRServerDriverHost()->TrackedDeviceAdded(hmd_->serial(), vr::TrackedDeviceClass_HMD, hmd_.get())) {
		log("TrackedDeviceAdded failed for the HMD");
		hmd_.reset();
		xr_.reset();
		return vr::VRInitError_Driver_Unknown;
	}

	// Touch controllers, when Monado gave us controller input.
	if (xr_ && xr_->input_ready() && vr::VRSettings()->GetBool("driver_monadolink", "controllers")) {
		reload_controller_offsets();
		tuning_enabled_ = vr::VRSettings()->GetBool("driver_monadolink", "controller_tuning");
		const std::string profile = get_setting_string("driver_monadolink", "controller_input_profile");
		for (int hand = 0; hand < 2; hand++) {
			std::string serial = std::string(hmd_->serial()) + (hand == k_left ? "_Controller_Left" : "_Controller_Right");
			controllers_[hand] = std::make_unique<ControllerDevice>((Hand)hand, serial, profile);
			if (!vr::VRServerDriverHost()->TrackedDeviceAdded(controllers_[hand]->serial(),
			                                                  vr::TrackedDeviceClass_Controller,
			                                                  controllers_[hand].get())) {
				log("TrackedDeviceAdded failed for the %s controller", hand == k_left ? "left" : "right");
				controllers_[hand].reset();
			}
		}
		log("Controllers added: input profile %s", profile.c_str());
	}

	if (xr_) {
		stop_tracking_ = false;
		tracking_thread_ = std::thread(&DeviceProvider::tracking_loop, this);
	}
	return vr::VRInitError_None;
}

void
DeviceProvider::tracking_loop()
{
	bool reported_loss = false;
	auto next_report = std::chrono::steady_clock::now();
	auto next_eye_check = next_report;
	while (!stop_tracking_) {
		xr_->poll_events();
		vr::DriverPose_t pose;
		if (xr_->locate_head(&pose)) {
			hmd_->update_pose(pose);
			// A sign of life in vrserver.txt: is tracking flowing, and where is the head?
			if (std::chrono::steady_clock::now() >= next_report) {
				log("head: valid %d result %d pos %+.2f %+.2f %+.2f quat w%+.2f x%+.2f y%+.2f z%+.2f",
				    pose.poseIsValid, (int)pose.result, pose.vecPosition[0], pose.vecPosition[1],
				    pose.vecPosition[2], pose.qRotation.w, pose.qRotation.x, pose.qRotation.y, pose.qRotation.z);
				next_report += std::chrono::seconds(5);
			}
		} else if (xr_->lost() && !reported_loss) {
			// TODO: reconnect when Monado comes back; for now SteamVR must be restarted.
			log("Lost Monado: the HMD is now disconnected (restart SteamVR after restarting Monado)");
			pose = hmd_->GetPose();
			pose.poseIsValid = false;
			pose.deviceIsConnected = false;
			pose.result = vr::TrackingResult_Uninitialized;
			hmd_->update_pose(pose);
			reported_loss = true;
		}

		// The IPD can change (or only become known) after Init.
		if (std::chrono::steady_clock::now() >= next_eye_check) {
			next_eye_check = std::chrono::steady_clock::now() + std::chrono::seconds(1);
			XrPosef eyes[2];
			if (xr_->refresh_eye_poses(eyes)) {
				hmd_->update_eyes(eyes);
				log("Eye poses changed: IPD %.1f mm", (eyes[1].position.x - eyes[0].position.x) * 1000.f);
			}
		}

		if (controllers_[0] || controllers_[1]) {
			XrPosef offset[2];
			{
				std::lock_guard lock(offset_mutex_);
				offset[0] = controller_offset_[0];
				offset[1] = controller_offset_[1];
			}
			HandState hands[2];
			xr_->sync_hands(offset, hands); // all inactive if it fails or Monado is gone
			if (tuning_enabled_) {
				run_tuner(hands);
			}
			for (int hand = 0; hand < 2; hand++) {
				if (controllers_[hand]) {
					controllers_[hand]->update(hands[hand]);
				}
			}
		}
		std::this_thread::sleep_for(k_tracking_interval);
	}
}

void
DeviceProvider::run_tuner(HandState hands[2])
{
	float v[6];
	{
		std::lock_guard lock(offset_mutex_);
		memcpy(v, offset_settings_, sizeof(v));
	}
	const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	const TunerResult t = tuner_.update(hands, now, v);
	if (t.entered || t.left) {
		log("Controller offset tuning %s", t.entered ? "on (sticks nudge the offset, A resets, hold both "
		                                                "sticks clicked again to leave)"
		                                             : "off");
		// Entering: one long buzz; leaving: a short one.
		for (int hand = 0; hand < 2; hand++) {
			xr_->haptic((Hand)hand, t.entered ? 0.4f : 0.1f, 0.f, 0.8f);
		}
	}
	if (t.changed) {
		{
			std::lock_guard lock(offset_mutex_);
			memcpy(offset_settings_, v, sizeof(v));
			for (int hand = 0; hand < 2; hand++) {
				controller_offset_[hand] = controller_offset(&v[0], &v[3], hand == k_right);
			}
			tuned_dirty_ = true;
		}
		log("Controller grip offset tuned: %.3f %.3f %.3f m, pitch %.1f yaw %.1f roll %.1f deg", v[0], v[1], v[2],
		    v[3], v[4], v[5]);
		xr_->haptic((Hand)t.step_hand, 0.02f, 0.f, 0.5f); // a tick per step
	}
	if (t.reset) {
		std::lock_guard lock(offset_mutex_);
		reset_requested_ = true;
	}
	if (t.suppress) {
		for (int hand = 0; hand < 2; hand++) {
			const HandState s = hands[hand];
			hands[hand] = HandState{};
			hands[hand].active = s.active; // keep the pose: the point is to see it move
			hands[hand].pose = s.pose;
		}
	}
}

bool
DeviceProvider::save_tuned_offsets()
{
	static const char *const keys[6] = {"controller_offset_x",     "controller_offset_y",   "controller_offset_z",
	                                    "controller_offset_pitch", "controller_offset_yaw", "controller_offset_roll"};
	float v[6];
	bool reset, dirty;
	{
		std::lock_guard lock(offset_mutex_);
		reset = reset_requested_;
		dirty = tuned_dirty_ && !reset;
		memcpy(v, offset_settings_, sizeof(v));
		reset_requested_ = tuned_dirty_ = false;
		if (reset) {
			offsets_loaded_ = false; // reload even if the defaults equal what we had
		}
	}
	// Settings are only touched from RunFrame's thread.
	for (int i = 0; i < 6 && (reset || dirty); i++) {
		if (reset) {
			vr::VRSettings()->RemoveKeyInSection("driver_monadolink", keys[i]); // default.vrsettings applies
		} else {
			vr::VRSettings()->SetFloat("driver_monadolink", keys[i], v[i]);
		}
	}
	return reset;
}

void
DeviceProvider::Cleanup()
{
	log("Cleanup");
	stop_tracking_ = true;
	if (tracking_thread_.joinable()) {
		tracking_thread_.join();
	}
	controllers_[0].reset();
	controllers_[1].reset();
	hmd_.reset();
	xr_.reset();
	VR_CLEANUP_SERVER_DRIVER_CONTEXT();
}

bool
DeviceProvider::reload_controller_offsets()
{
	static const char *const keys[6] = {"controller_offset_x",     "controller_offset_y",   "controller_offset_z",
	                                    "controller_offset_pitch", "controller_offset_yaw", "controller_offset_roll"};
	float v[6];
	for (int i = 0; i < 6; i++) {
		v[i] = vr::VRSettings()->GetFloat("driver_monadolink", keys[i]);
	}
	std::lock_guard lock(offset_mutex_);
	if (tuned_dirty_ || (offsets_loaded_ && memcmp(v, offset_settings_, sizeof(v)) == 0)) {
		return false; // tuned in the headset since save_tuned_offsets(): the settings are behind
	}
	memcpy(offset_settings_, v, sizeof(v));
	for (int hand = 0; hand < 2; hand++) {
		controller_offset_[hand] = controller_offset(&v[0], &v[3], hand == k_right);
	}
	offsets_loaded_ = true;
	log("Controller grip offset: %.3f %.3f %.3f m, pitch %.1f yaw %.1f roll %.1f deg", v[0], v[1], v[2], v[3], v[4], v[5]);
	return true;
}

void
DeviceProvider::RunFrame()
{
	if (hmd_ && !xr_) {
		hmd_->run_frame(); // no Monado: keep the static pose alive
	}

	// Live controller offset tuning: save what was tuned in the headset, and pick up settings changed
	// by tools/tune.cpp twice a second.
	if (controllers_[0] || controllers_[1]) {
		const auto now = std::chrono::steady_clock::now();
		if (save_tuned_offsets() || now >= next_offset_check_) {
			reload_controller_offsets();
			next_offset_check_ = now + std::chrono::milliseconds(500);
		}
	}

	// Drain events; haptics for our controllers go to Monado.
	vr::VREvent_t event;
	while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
		if (event.eventType != vr::VREvent_Input_HapticVibration || !xr_) {
			continue;
		}
		// VREvent_t is packed: its data isn't 8-byte aligned, so copy it out rather than bind a reference.
		vr::VREvent_HapticVibration_t h;
		memcpy(&h, &event.data.hapticVibration, sizeof(h));
		for (const auto &c : controllers_) {
			if (c && c->owns_haptic(h.componentHandle)) {
				xr_->haptic(c->hand(), h.fDurationSeconds, h.fFrequency, h.fAmplitude);
			}
		}
	}
}

} // namespace ml
