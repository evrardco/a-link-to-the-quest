#include "device_provider.h"

#include "util/env_file.h"
#include "util/log.h"
#include "util/stderr_capture.h"
#include "xr/xr_math.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
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

std::string
monado_socket_path(const std::string &setting)
{
	if (!setting.empty()) {
		return setting;
	}
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	return std::string(runtime_dir != nullptr && runtime_dir[0] != '\0' ? runtime_dir : "/tmp") + "/monado_comp_ipc";
}

bool
unix_socket_listening(const std::string &path)
{
	// The kernel's table of bound unix sockets: a stale socket file left by a crashed Monado isn't in
	// it, and reading it doesn't connect to Monado (which would log a client coming and going).
	FILE *f = fopen("/proc/net/unix", "r");
	if (f == nullptr) {
		return access(path.c_str(), F_OK) == 0;
	}
	bool found = false;
	char line[4096];
	const std::string suffix = " " + path;
	while (!found && fgets(line, sizeof(line), f) != nullptr) {
		size_t n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
			line[--n] = '\0';
		}
		found = n >= suffix.size() && strcmp(line + n - suffix.size(), suffix.c_str()) == 0;
	}
	fclose(f);
	return found;
}

std::string
DeviceProvider::runtime_json()
{
	// The running Monado's own manifest (start-monado.sh writes it), else the setting (this project's
	// host build of the old fork, by default).
	const std::string running = monado_runtime_file_path(get_setting_string("driver_monadolink", "monado_runtime_file"));
	if (!running.empty() && access(running.c_str(), R_OK) == 0) {
		return running;
	}
	return get_setting_string("driver_monadolink", "openxr_runtime_json");
}

vr::EVRInitError
DeviceProvider::Init(vr::IVRDriverContext *driver_context)
{
	VR_INIT_SERVER_DRIVER_CONTEXT(driver_context);
	log("Init (%s)", process_tag());

	// SteamVR runs vrserver inside the Steam Linux Runtime container, which doesn't show the host's
	// whole XDG_RUNTIME_DIR: Monado's client then waits ~20 s for its socket and fails. Say so.
	monado_socket_ = monado_socket_path(get_setting_string("driver_monadolink", "monado_socket"));
	const std::string &socket = monado_socket_;
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
		connected = xr_->start(runtime_json().c_str(), vr::VRSettings()->GetBool("driver_monadolink", "submit_frames"));
		if (connected) {
			xr_->apply_display_info(&display);
		}
	}
	if (!connected) {
		// It used to give up here for the whole SteamVR session: a headset with no controllers and no
		// picture (seen 2026-11-09, e.g. when the Quest re-enumerated on USB while Monado started).
		log("Monado not reachable yet: the HMD shows as not tracking and connects as soon as Monado's socket "
		    "%s listens (accept Quest Link in the headset)",
		    socket.c_str());
		xr_->mark_lost();
		display_pending_ = true;
	}

	// FSR: games render smaller than Monado's swapchains, monadolink upscales (VkCopier::composite).
	display_ = display; // Monado's own size, for comparing after a reconnect
	// The tray's Settings window (settings.env) wins over steamvr.vrsettings.
	const std::string tray_path = quest_vr_settings_path(get_setting_string("driver_monadolink", "quest_vr_settings"));
	const auto tray = read_env_file(tray_path);
	bool quiet_logs = vr::VRSettings()->GetBool("driver_monadolink", "quiet_logs");
	if (auto it = tray.find("QUEST_VR_LOGGING"); it != tray.end() && !it->second.empty()) {
		quiet_logs = it->second == "minimal";
	}
	if (quiet_logs) {
		log("Minimal logging (quiet_logs / QUEST_VR_LOGGING=minimal): no periodic pose and timing lines");
	}
	if (xr_) {
		float scale = vr::VRSettings()->GetFloat("driver_monadolink", "fsr_render_scale");
		display.fsr_sharpness = vr::VRSettings()->GetFloat("driver_monadolink", "fsr_sharpness");
		auto tray_float = [&](const char *key, float *out) {
			auto it = tray.find(key);
			if (it != tray.end() && !it->second.empty()) {
				*out = strtof(it->second.c_str(), nullptr);
				log("%s = %s (from %s)", key, it->second.c_str(), tray_path.c_str());
			}
		};
		tray_float("MONADOLINK_FSR_RENDER_SCALE", &scale);
		tray_float("MONADOLINK_FSR_SHARPNESS", &display.fsr_sharpness);
		const uint32_t w = display.render_width, h = display.render_height;
		scale = apply_render_scale(&display, scale);
		fsr_scale_ = scale; // for a display that arrives late (reconnect)
		if (scale < 1.f) {
			log("FSR: SteamVR renders %ux%u per eye (scale %.2f, sharpness %.2f), upscaled to Monado's %ux%u",
			    display.render_width, display.render_height, scale, display.fsr_sharpness, w, h);
		}
	}
	hmd_ = std::make_unique<HmdDevice>(display, xr_.get());
	hmd_->set_quiet_logs(quiet_logs);
	if (!connected) {
		vr::DriverPose_t pose = hmd_->GetPose();
		pose.poseIsValid = false;
		pose.deviceIsConnected = false;
		pose.result = vr::TrackingResult_Uninitialized;
		hmd_->update_pose(pose);
	}
	if (!vr::VRServerDriverHost()->TrackedDeviceAdded(hmd_->serial(), vr::TrackedDeviceClass_HMD, hmd_.get())) {
		log("TrackedDeviceAdded failed for the HMD");
		hmd_.reset();
		xr_.reset();
		return vr::VRInitError_Driver_Unknown;
	}

	// Touch controllers, when Monado gave us controller input (or isn't there yet: they stay inactive
	// until it is).
	if (xr_ && (!connected || xr_->input_ready()) && vr::VRSettings()->GetBool("driver_monadolink", "controllers")) {
		reload_controller_offsets();
		tuning_enabled_ = vr::VRSettings()->GetBool("driver_monadolink", "controller_tuning");
		stream_reset_path_ = get_setting_string("driver_monadolink", "stream_reset_file");
		if (stream_reset_path_.empty() && getenv("HOME") != nullptr) {
			stream_reset_path_ = std::string(getenv("HOME")) + "/.cache/quest-vr/reset-stream";
		}
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

	const int interval_ms = vr::VRSettings()->GetInt32("driver_monadolink", "reconnect_interval_ms");
	reconnect_interval_ = std::chrono::milliseconds(interval_ms > 0 ? interval_ms : 2000);
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
	auto next_reconnect = std::chrono::steady_clock::now();
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
			if (!display_pending_) {
				log("Lost Monado: the HMD is now disconnected; reconnecting when Monado's socket %s is back",
				    monado_socket_.c_str());
			}
			pose = hmd_->GetPose();
			pose.poseIsValid = false;
			pose.deviceIsConnected = false;
			pose.result = vr::TrackingResult_Uninitialized;
			hmd_->update_pose(pose);
			reported_loss = true;
			next_reconnect = std::chrono::steady_clock::now() + reconnect_interval_;
		} else if (xr_->lost() && std::chrono::steady_clock::now() >= next_reconnect) {
			next_reconnect = std::chrono::steady_clock::now() + reconnect_interval_;
			// Only once Monado listens again (it opens the socket after Quest Link is accepted): with no
			// socket, xrCreateInstance would block this thread for ~20 s.
			if (unix_socket_listening(monado_socket_)) {
				if (reconnect()) {
					reported_loss = false; // tracking resumes: the next pose is valid and connected
				}
			}
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

bool
DeviceProvider::reconnect()
{
	log("Monado's socket is back: reconnecting");
	bool ok;
	DisplayConfig now = display_;
	{
		StderrCapture capture("OpenXR stderr: ");
		xr_->set_runtime_json(runtime_json()); // Monado may have been restarted from another build
		ok = xr_->restart();
		if (ok) {
			xr_->apply_display_info(&now);
		}
	}
	if (!ok) {
		log("Reconnecting to Monado failed: trying again in %lld ms", (long long)reconnect_interval_.count());
		return false;
	}
	if (display_pending_) {
		// First connection, after SteamVR started with the placeholder display: send Monado's.
		display_pending_ = false;
		display_ = now;
		DisplayConfig shown = now;
		shown.fsr_sharpness = hmd_->display_config().fsr_sharpness;
		apply_render_scale(&shown, fsr_scale_);
		hmd_->update_display(shown);
		log("Connected to Monado after SteamVR started: if the picture looks wrong (stretched, or only part "
		    "of it), restart SteamVR");
		if ((controllers_[0] || controllers_[1]) && !xr_->input_ready()) {
			log("Connected without controller input: the controllers stay inactive");
		}
		return true;
	}
	// SteamVR keeps the display it read at startup; a different one only scales or crops the image.
	if (now.render_width != display_.render_width || now.render_height != display_.render_height ||
	    std::fabs(now.refresh_rate - display_.refresh_rate) > 0.5f) {
		log("Reconnected, but Monado's display changed (%ux%u at %.0f Hz, was %ux%u at %.0f Hz): restart SteamVR "
		    "to render for it",
		    now.render_width, now.render_height, now.refresh_rate, display_.render_width, display_.render_height,
		    display_.refresh_rate);
	} else {
		log("Reconnected to Monado");
	}
	if ((controllers_[0] || controllers_[1]) && !xr_->input_ready()) {
		log("Reconnected without controller input: the controllers stay inactive");
	}
	return true;
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
		log("Controller offset tuning %s", t.entered ? "on (sticks nudge the offset, A resets, B resets the stream, "
		                                                "X: floor from a controller on the floor, Y: headset floor; "
		                                                "hold both sticks clicked again to leave)"
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
	if (t.reset_stream) {
		request_stream_reset();
		for (int hand = 0; hand < 2; hand++) {
			xr_->haptic((Hand)hand, 0.05f, 0.f, 0.8f);
		}
	}
	if (t.floor_from_controller) {
		set_floor_from_controllers(hands);
	}
	if (t.floor_reset) {
		xr_->set_floor_adjust_y(0.0);
		log("Floor height: back to the headset's own floor");
		for (int hand = 0; hand < 2; hand++) {
			xr_->haptic((Hand)hand, 0.05f, 0.f, 0.8f);
		}
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

void
DeviceProvider::set_floor_from_controllers(const HandState hands[2])
{
	int low = -1;
	for (int hand = 0; hand < 2; hand++) {
		if (hands[hand].active && (low < 0 || hands[hand].pose.vecPosition[1] < hands[low].pose.vecPosition[1])) {
			low = hand;
		}
	}
	const double old = xr_->floor_adjust_y();
	double adjust = 0.0;
	if (low < 0 || !floor_adjust_from_controller(old, hands[low].pose.vecPosition[1], &adjust)) {
		log("Floor height: not changed (%s)", low < 0 ? "no tracked controller"
		                                             : "it would move the floor by more than 1 m: is a controller on the floor?");
		xr_->haptic(low < 0 ? k_left : (Hand)low, 0.5f, 0.f, 1.f); // one long buzz: refused
		return;
	}
	xr_->set_floor_adjust_y(adjust);
	log("Floor height: from the %s controller at %.3f m, correction %+.3f m (was %+.3f)", low == k_left ? "left" : "right",
	    hands[low].pose.vecPosition[1], adjust, old);
	for (int hand = 0; hand < 2; hand++) {
		xr_->haptic((Hand)hand, 0.05f, 0.f, 0.8f);
	}
}

void
DeviceProvider::request_stream_reset()
{
	// Monado watches this file (QL_RESET_TRIGGER, xrsp_check_stream_reset): a new modification time
	// makes it send a keyframe. Our home folder is visible inside SteamVR's container.
	const std::string &path = stream_reset_path_;
	if (path.empty()) {
		log("Stream reset: no HOME, can't reach Monado's trigger file");
		return;
	}
	// mkdir -p of the parent folders (exists already is fine).
	for (size_t i = 1; i < path.size(); i++) {
		if (path[i] == '/') {
			mkdir(path.substr(0, i).c_str(), 0755);
		}
	}
	FILE *f = fopen(path.c_str(), "w");
	if (f == nullptr) {
		log("Stream reset: can't write %s", path.c_str());
		return;
	}
	fprintf(f, "%lld\n", (long long)std::chrono::steady_clock::now().time_since_epoch().count());
	fclose(f);
	log("Stream reset requested (%s)", path.c_str());
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
	if (hmd_ && (!xr_ || xr_->lost())) {
		hmd_->run_frame(); // no Monado (yet): keep publishing the last pose (not tracking)
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
