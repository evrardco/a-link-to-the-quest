// SPDX-License-Identifier: MIT
#include "direct/direct_mode.h"

#include "util/log.h"
#include "util/timing_stats.h"
#include "vk/vk_copy.h"
#include "xr/xr_client.h"
#include "xr/xr_math.h"

#include <thread>

static_assert(ml::DirectModeComponent::k_max_layers == ml::XrClient::k_max_layers, "one swapchain set per layer");
#include <unistd.h>

namespace ml {

//! Largest rotation between the pose SteamVR rendered a frame with and the tracked head pose before
//! the frame is dropped (pose guard). Normal prediction differences are a few degrees.
static constexpr float k_max_render_pose_error_deg = 20.f;
//! Largest difference between the projection tangents SteamVR rendered with and Monado's (~1 deg near
//! the centre) before a frame counts as rendered for another field of view.
static constexpr float k_max_projection_difference = 0.02f;

//! Timing statistics (util/timing_stats.h): ML_TIMING_STATS_INTERVAL_S = 0 turns them off.
static constexpr bool k_timing = ML_TIMING_STATS_INTERVAL_S > 0;
static constexpr int64_t k_timing_interval_ns = (int64_t)ML_TIMING_STATS_INTERVAL_S * 1'000'000'000;

static int64_t
now_ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

DirectModeComponent::DirectModeComponent(float refresh_rate, int log_frames, XrClient *xr, bool early_wait,
                                         bool overlay_layers)
    : frame_interval_(std::chrono::nanoseconds((int64_t)(1e9 / (refresh_rate > 0 ? refresh_rate : 120.f)))),
      next_vsync_(std::chrono::steady_clock::now()), log_frames_(log_frames), xr_(xr), overlay_layers_(overlay_layers),
      early_wait_(early_wait),
      stats_(std::make_unique<TimingStats>(frame_interval_.count() * 3 / 2)), timing_(k_timing)
{
	if (xr_ != nullptr) {
		// Monado restarted: the copier's imports and pipelines live on the Vulkan device the client is
		// about to destroy. Runs on the tracking thread with the frame mutex held, so never during
		// submit_frame; the next frame imports SteamVR's textures again from the fds we keep.
		xr_->set_teardown_hook([this] {
			copier_.reset();
			copier_failed_ = false;
			composite_disabled_ = false;
			waited_ = false;
			std::lock_guard lock(mutex_);
			to_forget_.clear(); // the imports they refer to are gone with the copier
			import_failed_.clear();
			log("Monado reconnecting: dropped our Vulkan copies of SteamVR's textures");
		});
	}
}

void
DirectModeComponent::set_overlay_debug(int mode)
{
	overlay_debug_ = mode;
	if (mode != 0) {
		log("overlay_debug %d: %s", mode,
		    mode == 1   ? "every layer copied into its own swapchains, only the scene submitted"
		    : mode == 2 ? "separate layers submitted to Monado"
		                : "unknown mode (separate layers)");
	}
}

DirectModeComponent::~DirectModeComponent()
{
	copier_.reset(); // before the fds: Vulkan holds its own references to the imported memory
	for (auto &set : sets_) {
		for (int fd : set.fds) {
			if (fd >= 0) {
				close(fd);
			}
		}
	}
}

void
DirectModeComponent::CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *desc, SwapTextureSet_t *out_set)
{
	std::lock_guard lock(mutex_);

	SwapSet set{};
	set.pid = pid;
	set.desc = *desc;
	set.fds.fill(-1);
	set.reffed.fill(false);

	// SteamVR allocates and tracks the images; we get IPC handles. On Linux nFormat is a VkFormat.
	// The extra flags/usage must match how VkCopier imports them (an opaque-fd import must match).
	for (int i = 0; i < k_textures_per_set; i++) {
		bool ok = vr::VRIPCResourceManager()->NewSharedVulkanImage(
		    desc->nFormat, desc->nWidth, desc->nHeight, /*renderable*/ true, /*mappable*/ false,
		    /*compute access (sRGB formats can't be storage images)*/ false, /*mips*/ 1, /*array layers*/ 1,
		    k_shared_image_flags, k_shared_image_usage, &set.handles[i]);
		if (!ok) {
			log("NewSharedVulkanImage failed (format %u, %ux%u)", desc->nFormat, desc->nWidth, desc->nHeight);
			for (int j = 0; j < i; j++) {
				vr::VRIPCResourceManager()->UnrefResource(set.handles[j]);
				if (set.reffed[j]) {
					vr::VRIPCResourceManager()->UnrefResource(set.handles[j]);
				}
				if (set.fds[j] >= 0) {
					close(set.fds[j]);
				}
			}
			*out_set = {};
			return;
		}
		// An fd for the image, to import it into our Vulkan device.
		uint64_t ipc_handle = 0;
		if (vr::VRIPCResourceManager()->RefResource(set.handles[i], &ipc_handle)) {
			set.reffed[i] = true;
			int fd = -1;
			if (vr::VRIPCResourceManager()->ReceiveSharedFd(ipc_handle, &fd)) {
				set.fds[i] = fd;
			}
		}
		out_set->rSharedTextureHandles[i] = set.handles[i];
	}
	out_set->unTextureFlags = 0;

	log("CreateSwapTextureSet for pid %u: VkFormat %u, %ux%u, %u samples -> handles %llu/%llu/%llu, fds %d/%d/%d "
	    "(%s)",
	    pid, desc->nFormat, desc->nWidth, desc->nHeight, desc->nSampleCount, (unsigned long long)set.handles[0],
	    (unsigned long long)set.handles[1], (unsigned long long)set.handles[2], set.fds[0], set.fds[1], set.fds[2],
	    process_tag());
	sets_.push_back(set);
}

DirectModeComponent::SwapSet *
DirectModeComponent::find_set_locked(vr::SharedTextureHandle_t handle, int *out_index)
{
	for (auto &set : sets_) {
		for (int i = 0; i < k_textures_per_set; i++) {
			if (set.handles[i] == handle) {
				if (out_index != nullptr) {
					*out_index = i;
				}
				return &set;
			}
		}
	}
	return nullptr;
}

void
DirectModeComponent::destroy_set_locked(size_t i)
{
	SwapSet &set = sets_[i];
	for (int t = 0; t < k_textures_per_set; t++) {
		vr::VRIPCResourceManager()->UnrefResource(set.handles[t]);
		if (set.reffed[t]) {
			vr::VRIPCResourceManager()->UnrefResource(set.handles[t]);
		}
		if (set.fds[t] >= 0) {
			close(set.fds[t]);
		}
		to_forget_.push_back(set.handles[t]); // the copier drops its import on the submit thread
		import_failed_.erase(set.handles[t]);
	}
	sets_.erase(sets_.begin() + i);
}

void
DirectModeComponent::DestroySwapTextureSet(vr::SharedTextureHandle_t handle)
{
	std::lock_guard lock(mutex_);
	for (size_t i = 0; i < sets_.size(); i++) {
		for (auto h : sets_[i].handles) {
			if (h == handle) {
				log("DestroySwapTextureSet (pid %u)", sets_[i].pid);
				destroy_set_locked(i);
				return;
			}
		}
	}
}

void
DirectModeComponent::DestroyAllSwapTextureSets(uint32_t pid)
{
	std::lock_guard lock(mutex_);
	for (size_t i = sets_.size(); i-- > 0;) {
		if (sets_[i].pid == pid) {
			destroy_set_locked(i);
		}
	}
	log("DestroyAllSwapTextureSets for pid %u", pid);
}

void
DirectModeComponent::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2])
{
	std::lock_guard lock(mutex_);
	for (int eye = 0; eye < 2; eye++) {
		SwapSet *set = find_set_locked(handles[eye], nullptr);
		if (set == nullptr) {
			(*indices)[eye] = 0;
			continue;
		}
		// Both eyes may share one set; advance each set once per call.
		if (eye == 1 && handles[1] == handles[0]) {
			(*indices)[1] = (*indices)[0];
			continue;
		}
		set->current = (set->current + 1) % k_textures_per_set;
		(*indices)[eye] = set->current;
	}
}

void
DirectModeComponent::SubmitLayer(const SubmitLayerPerEye_t (&per_eye)[2])
{
	if (should_log()) {
		std::lock_guard lock(mutex_);
		for (int eye = 0; eye < 2; eye++) {
			const auto &l = per_eye[eye];
			int index = -1;
			SwapSet *set = find_set_locked(l.hTexture, &index);
			log("frame %llu layer %u eye %d: texture %llu (set pid %d, index %d), depth %llu, bounds u[%.3f..%.3f] "
			    "v[%.3f..%.3f], pose predicted %.1f ms ahead (%s)",
			    (unsigned long long)frame_, layers_this_frame_, eye, (unsigned long long)l.hTexture,
			    set ? (int)set->pid : -1, index, (unsigned long long)l.hDepthTexture, l.bounds.uMin, l.bounds.uMax,
			    l.bounds.vMin, l.bounds.vMax, l.flHmdPosePredictionTimeInSecondsFromNow * 1000.f, process_tag());
		}
	}
	if (layers_this_frame_ == 0) {
		const int64_t predict_ns = (int64_t)(per_eye[0].flHmdPosePredictionTimeInSecondsFromNow * 1e9);
		steamvr_display_ns_ = now_ns() + predict_ns;
		if (timing_) {
			stats_->add(TimingStats::Predict, predict_ns);
		}
	}
	// Every layer (M3): the scene first, then SteamVR's overlays (dashboard, keyboard, ...).
	if (layers_this_frame_ < (uint32_t)k_max_layers) {
		layers_[layers_this_frame_][0] = per_eye[0];
		layers_[layers_this_frame_][1] = per_eye[1];
		layer_textures_[layers_this_frame_].insert(per_eye[0].hTexture);
	} else if (!logged_layer_overflow_) {
		logged_layer_overflow_ = true;
		log("SteamVR submitted more than %d layers in a frame: the extra ones aren't shown", k_max_layers);
	}
	layers_this_frame_++;
}

bool
DirectModeComponent::import_texture(vr::SharedTextureHandle_t handle)
{
	int fd = -1;
	SwapTextureSetDesc_t desc{};
	{
		std::lock_guard lock(mutex_); // import_failed_ too: SteamVR's threads and the teardown hook change it
		if (import_failed_.count(handle) != 0) {
			return false;
		}
		int index = -1;
		SwapSet *set = find_set_locked(handle, &index);
		if (set == nullptr || set->fds[index] < 0) {
			import_failed_.insert(handle);
			log("No fd for SteamVR texture %llu: can't show it", (unsigned long long)handle);
			return false;
		}
		fd = dup(set->fds[index]); // keep ours: the import consumes the fd it gets
		desc = set->desc;
	}
	if (!copier_->import(handle, fd, (VkFormat)desc.nFormat, desc.nWidth, desc.nHeight)) {
		std::lock_guard lock(mutex_);
		import_failed_.insert(handle);
		return false;
	}
	return true;
}

bool
DirectModeComponent::submit_frame()
{
	if (xr_ == nullptr || layers_this_frame_ == 0) {
		return false;
	}
	// try_lock: while the client restarts (it holds the mutex for its teardown) SteamVR mustn't block
	// here; PostPresent then paces the frame itself.
	std::unique_lock frame_lock(xr_->frame_mutex(), std::try_to_lock);
	if (!frame_lock.owns_lock() || xr_->restarting() || !xr_->frames_enabled() || copier_failed_) {
		waited_ = false;
		return false;
	}
	if (!xr_->running()) {
		waited_ = false; // a frame waited before the session stopped is gone with it
		return false;
	}

	if (!copier_) {
		copier_ = std::make_unique<VkCopier>(xr_->vk());
		if (!copier_->init()) {
			log("Vulkan copier setup failed: frames won't be shown");
			copier_.reset();
			copier_failed_ = true;
			return false;
		}
		copier_->set_upscale(upscale_, sharpness_);
	}
	{
		std::lock_guard lock(mutex_);
		for (auto handle : to_forget_) {
			copier_->forget(handle); // our last copy has completed: nothing uses them on the GPU
		}
		to_forget_.clear();
	}

	// The layers we can show, bottom first: the scene must be importable, an overlay that isn't (or
	// whose swapchains can't be created) is left out. Swapchain set i holds shown layer i.
	const int max_shown = overlay_layers_ ? k_max_layers : 1;
	const int submitted = layers_this_frame_ < (uint32_t)max_shown ? (int)layers_this_frame_ : max_shown;
	if ((uint32_t)submitted > max_layers_seen_) {
		max_layers_seen_ = submitted;
		log("SteamVR submits %d layers per frame now (frame %llu)", submitted, (unsigned long long)frame_);
	}
	// Overlays are composited into the scene's swapchain image (VkCopier::composite) and Monado gets
	// one layer; overlay_debug != 0 hands Monado separate layers instead (its multi-layer path showed
	// them as a magnified smear on Quest Link).
	const bool separate_layers = overlay_debug_ != 0;
	int shown_layer[k_max_layers];
	int n = 0;
	for (int l = 0; l < submitted; l++) {
		bool ok = true;
		for (int eye = 0; eye < 2; eye++) {
			const auto handle = layers_[l][eye].hTexture;
			ok = ok && handle != 0 && (copier_->has(handle) || import_texture(handle));
		}
		ok = ok && (!separate_layers || xr_->ensure_layer(n));
		if (ok) {
			shown_layer[n++] = l;
		} else if (l == 0) {
			break; // no scene: nothing to show
		}
	}
	const bool have_textures = n > 0 && shown_layer[0] == 0;
	// FSR upscaling also goes through the composite, with the scene alone.
	const bool composite = (n > 1 || upscale_) && n > 0 && !separate_layers && !composite_disabled_;
	const int sets = composite ? 1 : separate_layers ? n : 1; // swapchain sets used this frame

	const int64_t t_start = now_ns();
	XrTime display_time = 0;
	bool should_render = false;
	if (waited_) { // early wait: PostPresent already waited for this frame
		display_time = waited_display_time_;
		should_render = waited_should_render_;
		waited_ = false;
	} else if (!wait_frame_locked(&display_time, &should_render)) {
		return false;
	}
	if (!xr_->begin_frame()) {
		return false;
	}
	int64_t t = now_ns();
	if (timing_) {
		stats_->add(TimingStats::Mismatch, steamvr_display_ns_ - monado_display_ns_);
	}
	if (!should_render || !have_textures) {
		xr_->end_frame(display_time, nullptr);
		if (timing_) {
			stats_->count_dropped();
			stats_->add(TimingStats::Total, now_ns() - t_start);
		}
		return true;
	}

	uint32_t index[k_max_layers][2] = {};
	bool acquired[k_max_layers][2] = {};
	bool all_acquired = true;
	for (int i = 0; i < sets; i++) {
		for (int eye = 0; eye < 2; eye++) {
			acquired[i][eye] = xr_->acquire(i, eye, &index[i][eye]);
			all_acquired = all_acquired && acquired[i][eye];
		}
	}
	if (timing_) {
		const int64_t t2 = now_ns();
		stats_->add(TimingStats::Acquire, t2 - t);
		t = t2;
	}
	bool copied = false;
	if (all_acquired && composite) {
		VkCopier::CompositeEye eyes[2] = {};
		for (int eye = 0; eye < 2; eye++) {
			eyes[eye].count = n;
			for (int i = 0; i < n; i++) {
				const auto &l = layers_[shown_layer[i]][eye];
				eyes[eye].src[i] = l.hTexture;
				eyes[eye].rect[i][0] = l.bounds.uMin;
				eyes[eye].rect[i][1] = l.bounds.vMin;
				eyes[eye].rect[i][2] = l.bounds.uMax;
				eyes[eye].rect[i][3] = l.bounds.vMax;
			}
			eyes[eye].dst = xr_->swapchain_image(0, eye, index[0][eye]);
			eyes[eye].dst_width = xr_->swapchain_width(eye);
			eyes[eye].dst_height = xr_->swapchain_height(eye);
		}
		copied = copier_->composite(eyes);
		if (!copied && !composite_disabled_) {
			composite_disabled_ = true; // the copier logged why; the scene alone from now on
			log("Layer compositing failed: showing SteamVR's scene layer only from now on%s",
			    upscale_ ? " (stretched bilinearly, without FSR)" : "");
		}
		if (copied && upscale_ && !logged_upscale_) {
			logged_upscale_ = true;
			std::lock_guard lock(mutex_);
			SwapSet *set = find_set_locked(layers_[0][0].hTexture, nullptr);
			log("FSR upscaling %s: SteamVR's %ux%u -> %ux%u per eye", copier_->upscaled() ? "on" : "not used",
			    set ? set->desc.nWidth : 0, set ? set->desc.nHeight : 0, xr_->swapchain_width(0),
			    xr_->swapchain_height(0));
		}
		if (timing_) {
			stats_->add(TimingStats::Copy, now_ns() - t);
		}
	}
	if (all_acquired && !copied) {
		// The scene (and with overlay_debug, every layer into its own swapchain set).
		const int copy_layers = separate_layers ? n : 1;
		VkCopier::Blit blits[2 * k_max_layers];
		{
			std::lock_guard lock(mutex_);
			for (int i = 0; i < copy_layers; i++) {
				for (int eye = 0; eye < 2; eye++) {
					const auto &l = layers_[shown_layer[i]][eye];
					SwapSet *set = find_set_locked(l.hTexture, nullptr);
					const uint32_t w = set ? set->desc.nWidth : 0, h = set ? set->desc.nHeight : 0;
					VkCopier::Blit &b = blits[2 * i + eye];
					b.src = l.hTexture;
					rect_from_bounds(l.bounds, w, h, b.src_rect);
					b.dst = xr_->swapchain_image(i, eye, index[i][eye]);
					b.dst_width = xr_->swapchain_width(eye);
					b.dst_height = xr_->swapchain_height(eye);
				}
			}
		}
		// Diagnostics: the scene on logged frames; with overlays, one layer every 121 frames (121, not
		// 120: SteamVR cycles 3 textures per set, so each report lands on another one), alternating
		// between the layers.
		const bool overlay_report = !quiet_ && copy_layers > 1 && frame_ % 121 == 60;
		const int report_layer = overlay_report ? (int)(overlay_reports_++ % (uint64_t)copy_layers) : 0;
		const int sample_blit = should_log() ? 0 : overlay_report ? 2 * report_layer : -1;
		copied = copier_->copy(blits, 2 * copy_layers, sample_blit);
		if (timing_) {
			stats_->add(TimingStats::Copy, now_ns() - t);
		}
		if (copied && overlay_report) {
			const auto &px = copier_->samples();
			char layers[512] = "";
			int off = 0;
			for (int i = 0; i < n && off < (int)sizeof(layers); i++) {
				const auto &l = layers_[shown_layer[i]][0];
				uint32_t format = 0;
				int index = -1;
				{
					std::lock_guard lock(mutex_);
					SwapSet *set = find_set_locked(l.hTexture, &index);
					format = set ? set->desc.nFormat : 0;
				}
				float pl = 0, pr = 0, pt = 0, pb = 0;
				const bool have_proj = tangents_from_projection(l.mProjection, &pl, &pr, &pt, &pb);
				off += snprintf(layers + off, sizeof(layers) - off,
				                " | layer %d: texture index %d, VkFormat %u, %zu textures, bounds u %.2f..%.2f v %.2f..%.2f, "
				                "proj l/r/t/b %s%.3f/%.3f/%.3f/%.3f, predicted %.1f ms",
				                shown_layer[i], index, format, layer_textures_[shown_layer[i]].size(), l.bounds.uMin,
				                l.bounds.uMax, l.bounds.vMin, l.bounds.vMax, have_proj ? "" : "(none) ", pl, pr, pt, pb,
				                l.flHmdPosePredictionTimeInSecondsFromNow * 1000.f);
			}
			float ml_, mr, mt, mb;
			projection_from_xr_fov(xr_->fov(0), &ml_, &mr, &mt, &mb);
			log("frame %llu layer %d sampled: bytes at centre %u,%u,%u,%u, quarters %u,%u,%u,%u / %u,%u,%u,%u / "
			    "%u,%u,%u,%u / %u,%u,%u,%u; Monado's left eye l/r/t/b %.3f/%.3f/%.3f/%.3f%s",
			    (unsigned long long)frame_, shown_layer[report_layer], px[0][0], px[0][1], px[0][2], px[0][3],
			    px[1][0], px[1][1], px[1][2], px[1][3], px[2][0], px[2][1], px[2][2], px[2][3], px[3][0], px[3][1],
			    px[3][2], px[3][3], px[4][0], px[4][1], px[4][2], px[4][3], ml_, mr, mt, mb, layers);
			for (auto &tex : layer_textures_) {
				tex.clear();
			}
		}
		if (copied && sample_blit == 0) {
			// What SteamVR rendered (left eye, centre + quarter points): tells a flat image from SteamVR
			// apart from one our copy produced.
			const auto &px = copier_->samples();
			log("frame %llu left texture RGBA at centre %u,%u,%u,%u; quarters %u,%u,%u / %u,%u,%u / %u,%u,%u / "
			    "%u,%u,%u",
			    (unsigned long long)frame_, px[0][0], px[0][1], px[0][2], px[0][3], px[1][0], px[1][1], px[1][2],
			    px[2][0], px[2][1], px[2][2], px[3][0], px[3][1], px[3][2], px[4][0], px[4][1], px[4][2]);
		}
	}
	for (int i = 0; i < sets; i++) {
		for (int eye = 0; eye < 2; eye++) {
			if (acquired[i][eye]) {
				xr_->release(i, eye);
			}
		}
	}

	// Safety: the pose SteamVR rendered with must be close to where the head really is. If it isn't
	// (a pose bug: the world would swim or stick to the face, which makes people sick), show nothing.
	XrPosef render_head = pose_from_matrix(layers_[0][0].mHmdPose);
	render_head.position.y -= (float)xr_->world_offset_y();
	XrPosef tracked_head{};
	const bool have_tracked = xr_->last_head(&tracked_head);
	const float error_deg = have_tracked ? rotation_angle_deg(render_head.orientation, tracked_head.orientation) : 0.f;
	bool pose_ok = have_tracked && error_deg <= k_max_render_pose_error_deg;

	// Same idea for the field of view: SteamVR must have rendered with Monado's projection. Games that
	// pass no projection matrix can't be checked.
	float fov_diff = 0.f;
	for (int eye = 0; eye < 2; eye++) {
		float d = 0.f;
		if (projection_difference(layers_[0][eye].mProjection, xr_->fov(eye), &d)) {
			fov_diff = std::fmax(fov_diff, d);
		}
	}
	if (fov_diff > k_max_projection_difference) {
		const bool drop = projection_enforced_;
		if (projection_mismatches_++ % 600 == 0) {
			log("frame %llu: SteamVR rendered with another field of view than Monado's (tangents differ by %.3f)%s",
			    (unsigned long long)frame_, fov_diff,
			    drop ? " -> FRAME NOT SHOWN (projection guard): restart SteamVR" : "");
		}
		if (drop) {
			pose_ok = false;
		}
	}
	if (should_log() || (!quiet_ && frame_ % 120 == 0) || (!pose_ok && rejected_frames_ % 120 == 0)) { // ~1 s at 120 Hz
		log("frame %llu render pose pos %+.2f %+.2f %+.2f quat w%+.3f x%+.3f y%+.3f z%+.3f | tracked pos %+.2f %+.2f "
		    "%+.2f quat w%+.3f x%+.3f y%+.3f z%+.3f | differ by %.1f deg%s",
		    (unsigned long long)frame_, render_head.position.x, render_head.position.y, render_head.position.z,
		    render_head.orientation.w, render_head.orientation.x, render_head.orientation.y,
		    render_head.orientation.z, tracked_head.position.x, tracked_head.position.y, tracked_head.position.z,
		    tracked_head.orientation.w, tracked_head.orientation.x, tracked_head.orientation.y,
		    tracked_head.orientation.z, error_deg,
		    pose_ok ? "" : have_tracked ? " -> FRAME NOT SHOWN (pose guard)" : " -> not shown (no tracked pose yet)");
	}
	if (!pose_ok) {
		rejected_frames_++;
	}

	const int64_t t_end = now_ns();
	bool shown = false;
	if (copied && pose_ok) {
		// The scene's pose (composited frames: the overlays were rendered for nearly the same pose); with
		// overlay_debug, each layer with the pose SteamVR rendered it with, blended by Monado.
		const int submit = separate_layers && overlay_debug_ != 1 ? n : 1;
		const XrPosef eyes_in_head[2] = {xr_->eye_pose(0), xr_->eye_pose(1)};
		XrClient::Layer xl[k_max_layers];
		for (int i = 0; i < submit; i++) {
			layer_eye_poses(layers_[shown_layer[i]][0].mHmdPose, xr_->world_offset_y(), eyes_in_head, xl[i].eye_poses);
			xl[i].blend = i > 0;
		}
		shown = xr_->end_frame(display_time, xl, submit);
		if (timing_ && shown) {
			stats_->add(TimingStats::Margin, monado_display_ns_ - now_ns());
		}
		if (shown && frames_shown_++ == 0) {
			log("First frame shown in the headset (frame %llu)", (unsigned long long)frame_);
		}
	} else {
		xr_->end_frame(display_time, nullptr);
	}
	if (timing_) {
		const int64_t t_done = now_ns();
		stats_->add(TimingStats::End, t_done - t_end);
		stats_->add(TimingStats::Total, t_done - t_start);
		if (shown) {
			stats_->count_shown();
		} else {
			stats_->count_dropped();
		}
	}
	return true;
}

void
DirectModeComponent::Present(vr::SharedTextureHandle_t sync_texture)
{
	if (should_log()) {
		log("frame %llu Present: %u layer(s), sync texture %llu, %llu shown so far (%s)", (unsigned long long)frame_,
		    layers_this_frame_, (unsigned long long)sync_texture, (unsigned long long)frames_shown_, process_tag());
	}
	if (timing_) {
		const int64_t now = now_ns();
		if (last_present_ns_ != 0) {
			stats_->add(TimingStats::Gap, now - last_present_ns_);
		}
		last_present_ns_ = now;
		stats_->count_present();
	}
	frame_paced_ = submit_frame();
	layers_this_frame_ = 0;
	frame_++;
	if (timing_) {
		const int64_t now = now_ns();
		if (stats_->due(now, k_timing_interval_ns)) {
			char line[512];
			stats_->report(line, sizeof(line), now);
			log("%s", line);
		}
	}
}

bool
DirectModeComponent::wait_frame_locked(XrTime *display_time, bool *should_render)
{
	const int64_t t0 = now_ns();
	if (!xr_->wait_frame(display_time, should_render)) {
		return false;
	}
	const int64_t t1 = now_ns();
	XrTime xr_now = 0;
	const int64_t ahead = xr_->now(&xr_now) ? *display_time - xr_now : 0;
	monado_display_ns_ = t1 + ahead;
	if (timing_) {
		stats_->add(TimingStats::Wait, t1 - t0);
		stats_->add(TimingStats::Ahead, ahead);
	}
	return true;
}

void
DirectModeComponent::PostPresent(const Throttling_t *throttling)
{
	// Early wait: wait for Monado's next frame now, so SteamVR renders its next frame inside Monado's
	// window and Present can submit it right away. Waiting in Present instead held every finished
	// frame ~7 ms before it went to Monado.
	if (early_wait_ && frame_paced_ && !waited_) {
		std::unique_lock frame_lock(xr_->frame_mutex(), std::try_to_lock);
		if (frame_lock.owns_lock() && !xr_->restarting() && xr_->running() &&
		    wait_frame_locked(&waited_display_time_, &waited_should_render_)) {
			waited_ = true;
			next_vsync_ = std::chrono::steady_clock::now();
			return;
		}
	}
	if (frame_paced_) {
		next_vsync_ = std::chrono::steady_clock::now();
		return; // xrWaitFrame already held us until Monado wanted the next frame
	}
	// No OpenXR frame: pace to the refresh rate ourselves.
	auto now = std::chrono::steady_clock::now();
	next_vsync_ += frame_interval_;
	if (next_vsync_ < now) {
		next_vsync_ = now + frame_interval_; // fell behind: resynchronise instead of bursting
	}
	std::this_thread::sleep_until(next_vsync_);
}

void
DirectModeComponent::GetFrameTiming(vr::DriverDirectMode_FrameTiming *timing)
{
	// Must clear the reprojection flags if we don't fill them in (see openvr_driver.h).
	timing->m_nReprojectionFlags = 0;
}

} // namespace ml
