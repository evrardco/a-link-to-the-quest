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

class DirectModeComponent : public vr::IVRDriverDirectModeComponent
{
public:
	//! `xr` (may be null) submits frames to Monado; it must outlive this component. `early_wait`: run
	//! xrWaitFrame for the next frame in PostPresent instead of in Present (see DESIGN.md, M5: it made
	//! Monado's pacer run away to 30 fps under load, so it's off by default).
	DirectModeComponent(float refresh_rate, int log_frames, XrClient *xr = nullptr, bool early_wait = false);
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
		return frame_ < (uint64_t)log_frames_ || frame_ % 1200 == 0;
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
	SubmitLayerPerEye_t layer0_[2] = {}; //!< first layer of the current frame (M3: all layers)
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
	int64_t last_present_ns_ = 0;
};

} // namespace ml
