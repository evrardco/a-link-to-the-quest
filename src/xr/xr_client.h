// Our OpenXR client inside vrserver: an instance + session with Monado (or any OpenXR runtime).
//
// A Vulkan session (XR_KHR_vulkan_enable2) with one swapchain per eye, into which SteamVR's frames
// are copied (M2); if that isn't possible (or not wanted), a headless session (XR_MND_headless):
// display description and head tracking only (M1).
//
// Threading: start() and apply_display_info() run on the thread that calls Init. Afterwards the
// tracking thread calls poll_events()/locate_head(), and SteamVR's submit thread runs frames
// (wait_begin_frame .. end_frame) holding frame_mutex(), which poll_events also takes to begin/end
// the session. stop() runs once both are done.
#pragma once

#include "display_config.h"
#include "vk/vk_context.h"
#include "xr/xr_input.h"

#include <openvr_driver.h>

#include <ctime>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <mutex>
#include <vector>

#include <string>

namespace ml {

//! The runtime manifest the OpenXR loader will use (XR_RUNTIME_JSON or the first active_runtime.json
//! found), or "" if none.
std::string
runtime_manifest_path();
//! Whether a runtime manifest is SteamVR's own OpenXR runtime.
bool
is_steamvr_manifest(const std::string &path);

class XrClient
{
public:
	XrClient() = default;
	~XrClient();
	XrClient(const XrClient &) = delete;
	XrClient &
	operator=(const XrClient &) = delete;

	//! Connect: instance, HMD system, session (Vulkan + swapchains if `want_frames` and possible, else
	//! headless), reference spaces. `runtime_json` (if not empty) is exported as XR_RUNTIME_JSON first.
	//! False (logged) if the runtime isn't reachable or lacks what we need; the client is then stopped.
	bool
	start(const char *runtime_json, bool want_frames = true);
	//! Overwrite what the runtime knows in `c` (render size, FOV, eye poses/IPD, refresh rate).
	void
	apply_display_info(DisplayConfig *c);
	//! Handle pending events: begin/end the session, notice loss.
	void
	poll_events();
	//! The head pose now (CLOCK_MONOTONIC), in SteamVR's standing universe, with velocities.
	bool
	locate_head(vr::DriverPose_t *out);
	//! The last head pose locate_head() found, in the base space (thread-safe). False if none yet.
	bool
	last_head(XrPosef *out);
	//! Touch controllers (tracking thread): sync the actions and get both hands now, grip poses
	//! composed with `offset[hand]`. False if input isn't available (all hands inactive then).
	bool
	sync_hands(const XrPosef offset[2], HandState out[2]);
	//! Vibrate a controller (any thread).
	void
	haptic(Hand hand, float duration_s, float frequency, float amplitude)
	{
		input_.haptic(hand, duration_s, frequency, amplitude);
	}
	bool
	input_ready() const
	{
		return input_.ready();
	}
	//! The runtime went away (Monado stopped); nothing works until a new start().
	bool
	lost() const
	{
		return lost_;
	}
	void
	stop();

	// --- Frames (SteamVR's submit thread, holding frame_mutex()) -----------------------------------

	//! Whether frames can be submitted at all (Vulkan session with swapchains).
	bool
	frames_enabled() const
	{
		return vulkan_ && !swapchain_images_[0].empty() && !swapchain_images_[1].empty();
	}
	//! The session is running (begun and not stopping): frames may be submitted.
	bool
	running() const
	{
		return running_ && !lost_;
	}
	std::mutex &
	frame_mutex()
	{
		return frame_mutex_;
	}
	//! xrWaitFrame (blocks until the runtime wants the next frame: this paces SteamVR) + xrBeginFrame.
	bool
	wait_begin_frame(XrTime *display_time, bool *should_render)
	{
		return wait_frame(display_time, should_render) && begin_frame();
	}
	//! xrWaitFrame alone; begin_frame() must follow before the next wait_frame().
	bool
	wait_frame(XrTime *display_time, bool *should_render);
	//! xrBeginFrame for the frame wait_frame() returned.
	bool
	begin_frame();
	//! Acquire and wait for the eye's next swapchain image; released by release().
	bool
	acquire(int eye, uint32_t *index);
	void
	release(int eye);
	//! xrEndFrame with one projection layer (eye poses in the base space), or none if `eye_poses` is null.
	bool
	end_frame(XrTime display_time, const XrPosef *eye_poses);

	VkContext &
	vk()
	{
		return vk_;
	}
	VkImage
	swapchain_image(int eye, uint32_t index) const
	{
		return swapchain_images_[eye][index];
	}
	uint32_t
	swapchain_width(int eye) const
	{
		return swapchain_width_[eye];
	}
	uint32_t
	swapchain_height(int eye) const
	{
		return swapchain_height_[eye];
	}
	//! Eye pose relative to the head, from Monado (apply_display_info, refresh_eye_poses); any thread.
	XrPosef
	eye_pose(int eye)
	{
		std::lock_guard lock(eye_mutex_);
		return eye_pose_[eye];
	}
	//! Re-read the eye poses (the IPD can change: the Quest 2's slider, or a first real value after a
	//! bogus one). True with the new poses in `out` if they changed and are plausible.
	bool
	refresh_eye_poses(XrPosef out[2]);
	//! The current CLOCK_MONOTONIC time as an XrTime.
	bool
	now(XrTime *out);
	//! SteamVR's universe = base space lifted by this (0 for STAGE).
	double
	world_offset_y() const
	{
		return base_offset_y_;
	}

private:
	bool
	create_vulkan();
	bool
	create_swapchains();
	bool
	check(XrResult result, const char *what);
	//! xrLocateViews in VIEW space now: eye poses relative to the head (false if not valid).
	bool
	locate_eyes(XrPosef poses[2], XrFovf fovs[2]);

	XrInstance instance_ = XR_NULL_HANDLE;
	XrSystemId system_ = XR_NULL_SYSTEM_ID;
	XrSession session_ = XR_NULL_HANDLE;
	XrSpace base_space_ = XR_NULL_HANDLE; //!< STAGE (floor) when available, else LOCAL
	XrSpace view_space_ = XR_NULL_HANDLE;
	double base_offset_y_ = 0.0; //!< lifts LOCAL to standing height
	XrViewConfigurationView views_[2] = {};
	std::atomic<bool> running_{false};
	uint64_t locate_failures_ = 0;
	std::mutex head_mutex_;
	XrPosef last_head_{};
	bool have_head_ = false;

	XrInput input_;
	VkContext vk_;
	bool vulkan_ = false;
	std::mutex frame_mutex_;
	XrSwapchain swapchains_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	std::vector<VkImage> swapchain_images_[2];
	uint32_t swapchain_width_[2] = {}, swapchain_height_[2] = {};
	int64_t swapchain_format_ = 0;
	XrFovf fov_[2] = {};
	std::mutex eye_mutex_; //!< guards eye_pose_ (tracking thread writes, submit thread reads)
	XrPosef eye_pose_[2] = {{{0, 0, 0, 1}, {-0.032f, 0, 0}}, {{0, 0, 0, 1}, {0.032f, 0, 0}}};
	std::atomic<bool> lost_{false};

	PFN_xrConvertTimespecTimeToTimeKHR convert_timespec_ = nullptr;
	PFN_xrGetDisplayRefreshRateFB get_refresh_rate_ = nullptr;
};

} // namespace ml
