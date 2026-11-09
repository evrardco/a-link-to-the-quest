// IVRDriverDirectModeComponent: SteamVR's compositor renders into textures we allocate and hands
// us its layers every frame.
//
// Swap textures are allocated by SteamVR (IVRIPCResourceManagerClient::NewSharedVulkanImage); we get
// an fd for each (RefResource + ReceiveSharedFd) and, on the submit thread, import them into our
// Vulkan device. Present copies the first layer's eye textures into the OpenXR swapchains and ends
// the OpenXR frame with the pose SteamVR rendered with (M2); xrWaitFrame paces SteamVR. Without a
// frame-capable OpenXR session, Present only logs and PostPresent paces to the refresh rate (M0).
#pragma once

#include <openvr_driver.h>
#include <openxr/openxr.h>

#include <array>
#include <atomic>
#include <memory>
#include <set>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ml {

class TimingStats;
class VkCopier;
class XrClient;

//! Frames whose details are logged: the first `log_frames`, then one every 1200 (10 s at 120 Hz)
//! unless `quiet`.
inline bool
frame_logged(uint64_t frame, int log_frames, bool quiet)
{
	return frame < (uint64_t)log_frames || (!quiet && frame % 1200 == 0);
}

class DirectModeComponent : public vr::IVRDriverDirectModeComponent
{
public:
	//! Layers shown per frame (= XrClient::k_max_layers); SteamVR's extra layers are dropped (logged).
	static constexpr int k_max_layers = 4;

	//! `xr` (may be null) submits frames to Monado; it must outlive this component. `early_wait`: run
	//! xrWaitFrame for the next frame in PostPresent instead of in Present (see DESIGN.md, M5: it made
	//! Monado's pacer run away to 30 fps under load, so it's off by default).
	DirectModeComponent(float refresh_rate, int log_frames, XrClient *xr = nullptr, bool early_wait = false,
	                    bool overlay_layers = true);
	~DirectModeComponent();

	void
	CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *desc, SwapTextureSet_t *out_set) override;
	void
	DestroySwapTextureSet(vr::SharedTextureHandle_t handle) override;
	void
	DestroyAllSwapTextureSets(uint32_t pid) override;
	void
	GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) override;
	void
	SubmitLayer(const SubmitLayerPerEye_t (&per_eye)[2]) override;
	void
	Present(vr::SharedTextureHandle_t sync_texture) override;
	void
	PostPresent(const Throttling_t *throttling) override;
	void
	GetFrameTiming(vr::DriverDirectMode_FrameTiming *timing) override;

	//! Debug: setting overlay_debug (see overlay_debug_).
	void
	set_overlay_debug(int mode);
	//! Settings fsr_render_scale < 1 / fsr_sharpness: SteamVR renders smaller than the swapchains (the
	//! HMD reports the scaled size) and every frame goes through VkCopier::composite, which upscales
	//! with FSR 1. Call before the first frame.
	void
	set_upscale(bool on, float sharpness)
	{
		upscale_ = on;
		sharpness_ = sharpness;
	}
	//! Drop frames rendered with a projection other than Monado's (otherwise only logged). Set once
	//! Monado's display reached SteamVR late (HmdDevice::update_display), in case SteamVR kept the
	//! placeholder: a wrong field of view stretches the world, which makes people sick.
	void
	set_projection_enforced(bool on)
	{
		projection_enforced_ = on;
	}
	//! Minimal logging (setting quiet_logs, or the tray's QUEST_VR_LOGGING=minimal): only the first
	//! log_frames frames and problems are logged; no periodic pose and timing lines, no timing
	//! statistics and no diagnostic readbacks of SteamVR's textures. Call before the first frame.
	void
	set_quiet(bool quiet)
	{
		quiet_ = quiet;
		timing_ = timing_ && !quiet;
	}

private:
	static constexpr int k_textures_per_set = 3;

	struct SwapSet
	{
		uint32_t pid;
		SwapTextureSetDesc_t desc;
		std::array<vr::SharedTextureHandle_t, k_textures_per_set> handles;
		std::array<int, k_textures_per_set> fds;      //!< opaque Vulkan fds, -1 if unavailable
		std::array<bool, k_textures_per_set> reffed;  //!< RefResource succeeded: one more UnrefResource
		uint32_t current; //!< index SteamVR renders into next
	};

	SwapSet *
	find_set_locked(vr::SharedTextureHandle_t handle, int *out_index);
	void
	destroy_set_locked(size_t i);
	//! The OpenXR frame for the layers submitted since the last Present; false if none was run.
	bool
	submit_frame();
	//! xrWaitFrame (frame mutex held) + its timing statistics.
	bool
	wait_frame_locked(XrTime *display_time, bool *should_render);
	//! Import SteamVR's texture into the copier (submit thread).
	bool
	import_texture(vr::SharedTextureHandle_t handle);
	bool
	should_log() const
	{
		return frame_logged(frame_, log_frames_, quiet_);
	}

	std::mutex mutex_;
	std::vector<SwapSet> sets_;

	std::chrono::nanoseconds frame_interval_;
	std::chrono::steady_clock::time_point next_vsync_;
	int log_frames_;
	uint64_t frame_ = 0;
	uint32_t layers_this_frame_ = 0;

	XrClient *xr_;
	std::unique_ptr<VkCopier> copier_;   //!< created on the first frame (submit thread)
	bool copier_failed_ = false;
	SubmitLayerPerEye_t layers_[k_max_layers][2] = {}; //!< this frame's layers, bottom (the scene) first
	bool logged_layer_overflow_ = false;
	bool overlay_layers_;               //!< setting overlay_layers (default true); false: the scene layer only
	//! Setting overlay_debug: 0 = composite overlays ourselves (default); 1 = copy every layer into its
	//! own swapchains but submit the scene only; 2 = submit separate layers to Monado (broken there).
	int overlay_debug_ = 0;
	bool composite_disabled_ = false;   //!< compositing failed once: the scene alone
	bool upscale_ = false;              //!< FSR (set_upscale)
	float sharpness_ = 0.2f;
	bool logged_upscale_ = false;
	//! Diagnostics: textures each layer used since the last overlay report (every 120 frames).
	std::set<vr::SharedTextureHandle_t> layer_textures_[k_max_layers];
	uint64_t overlay_reports_ = 0;
	uint32_t max_layers_seen_ = 1;      //!< logged when it grows (the dashboard adds layers)
	std::vector<vr::SharedTextureHandle_t> to_forget_;  //!< destroyed by SteamVR, drop on the submit thread
	std::set<vr::SharedTextureHandle_t> import_failed_; //!< don't retry (and re-log) every frame
	bool frame_paced_ = false;           //!< the last Present ran xrWaitFrame: PostPresent needn't sleep
	bool early_wait_;
	bool waited_ = false;                //!< PostPresent ran xrWaitFrame: the next Present begins that frame
	XrTime waited_display_time_ = 0;
	bool waited_should_render_ = false;
	int64_t monado_display_ns_ = 0;      //!< the waited frame's display time, on steady_clock
	int64_t steamvr_display_ns_ = 0;     //!< when SteamVR predicted this frame's pose for, on steady_clock
	uint64_t frames_shown_ = 0;
	uint64_t rejected_frames_ = 0;       //!< dropped by the pose guard
	std::unique_ptr<TimingStats> stats_; //!< see util/timing_stats.h
	bool timing_;                        //!< collect and log stats_ (compiled in and not quiet_)
	bool quiet_ = false;                 //!< set_quiet
	std::atomic<bool> projection_enforced_{false}; //!< set_projection_enforced
	uint64_t projection_mismatches_ = 0;
	int64_t last_present_ns_ = 0;
};

} // namespace ml
