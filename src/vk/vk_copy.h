// Copies SteamVR's eye textures into OpenXR swapchain images, on our Vulkan queue.
//
// SteamVR's textures come as opaque Vulkan fds (IVRIPCResourceManagerClient::ReceiveSharedFd) of
// images it created with NewSharedVulkanImage; we import them with the same create parameters (see
// k_shared_image_* below). Before reading, the copy waits on the DMA-BUF's write fences (exported as
// a sync file) when the kernel and driver support it. The copy is synchronous: copy() returns once
// the GPU is done, so SteamVR can render into the texture again as soon as Present returns.
//
// Only call from one thread (SteamVR's submit thread), with the OpenXR frame lock held: the queue is
// shared with Monado's client compositor.
#pragma once

#include "vk/vk_context.h"

#include <cstdint>
#include <map>

namespace ml {

//! The extra create flags/usage we ask SteamVR for in NewSharedVulkanImage, and import with: an
//! opaque-fd import must match how the image was created. MUTABLE_FORMAT allows UNORM views of sRGB.
static constexpr uint32_t k_shared_image_flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
static constexpr uint32_t k_shared_image_usage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

class VkCopier
{
public:
	explicit VkCopier(VkContext &vk) : vk_(vk) {}
	~VkCopier();
	VkCopier(const VkCopier &) = delete;
	VkCopier &
	operator=(const VkCopier &) = delete;

	//! Command pool, command buffer, fence.
	bool
	init();
	//! Import SteamVR's texture `handle` from `fd` (ownership taken in all cases).
	bool
	import(uint64_t handle, int fd, VkFormat format, uint32_t width, uint32_t height);
	bool
	has(uint64_t handle) const
	{
		return images_.count(handle) != 0;
	}
	//! Drop an imported texture (only between copies: the GPU is idle for our work then).
	void
	forget(uint64_t handle);

	//! Most blits per copy(): k_max_layers (XrClient) layers × 2 eyes.
	static constexpr int k_max_blits = 8;

	struct Blit
	{
		uint64_t src;          //!< imported texture
		int32_t src_rect[4];   //!< x0, y0, x1, y1 in texels (x1 < x0 or y1 < y0 flips)
		VkImage dst;           //!< swapchain image, in COLOR_ATTACHMENT_OPTIMAL (OpenXR's rule)
		uint32_t dst_width, dst_height;
	};
	//! Record, submit and wait for the blits. False on failure (logged, rate-limited). With
	//! `sample_blit` >= 0, also read k_samples texels of that blit's source (see samples()).
	bool
	copy(const Blit *blits, int count, int sample_blit = -1);

	//! M3: SteamVR's layers composited into one image per eye (src/vk/composite.comp): the scene, then
	//! each overlay blended over it with straight alpha. Monado then gets one projection layer (its
	//! multi-layer path showed such frames as a magnified smear).
	static constexpr int k_composite_layers = 4;
	struct CompositeEye
	{
		int count;                                //!< layers, bottom (the scene) first
		uint64_t src[k_composite_layers];         //!< imported textures
		float rect[k_composite_layers][4];        //!< u0, v0, u1, v1: the layer's bounds (reversed = flipped)
		VkImage dst;                              //!< swapchain image, in COLOR_ATTACHMENT_OPTIMAL
		uint32_t dst_width, dst_height;
	};
	//! Composite both eyes, blit into their swapchain images, wait. False on failure (logged).
	bool
	composite(const CompositeEye eyes[2]);
	//! FSR 1 (src/vk/fsr_*.comp): composite() renders at the scene's size (at most the swapchain's) and
	//! upscales with EASU + RCAS (`sharpness` in stops, 0 = sharpest) instead of stretching bilinearly,
	//! every frame (a 1:1 pass when the scene is large enough). If its setup fails composite() goes on
	//! without it (logged once).
	void
	set_upscale(bool on, float sharpness)
	{
		upscale_ = on;
		sharpness_ = sharpness;
	}
	//! Whether the last composite() upscaled with FSR (diagnostics).
	bool
	upscaled() const
	{
		return upscaled_;
	}

	//! Diagnostics: texels of the sampled blit's source at its centre and quarter points, 4 bytes each
	//! in the texture's own channel order (RGBA, or BGRA for B8G8R8A8 formats).
	static constexpr int k_samples = 5;
	const uint8_t (&samples() const)[k_samples][4]
	{
		return samples_;
	}
	//! How many source textures were waited on through their DMA-BUF write fences (diagnostics).
	uint64_t
	fence_waits() const
	{
		return fence_waits_;
	}

private:
	struct Imported
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE; //!< sampled view (its own sRGB format: reads linear)
		int sync_fd = -1;          //!< a dup of the DMA-BUF, to export its write fences from
		bool initialized = false;  //!< moved out of UNDEFINED (into GENERAL) once
		uint32_t width = 0, height = 0;
	};

	void
	destroy(Imported &img);
	//! A semaphore signalled when the writers of `img` are done, or VK_NULL_HANDLE.
	VkSemaphore
	write_fence_semaphore(Imported &img, int slot);

	//! Compute pipeline for composite(), created on first use.
	bool
	init_composite();
	//! FSR's pipelines (EASU, RCAS), created on first use.
	bool
	init_fsr();
	bool
	fail(const char *what);

	struct Target
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		uint32_t width = 0, height = 0;
	};
	//! (Re)create `t` as a w x h linear float image with `usage`, unless it already is that size.
	bool
	ensure_image(Target &t, uint32_t w, uint32_t h, VkImageUsageFlags usage);
	void
	destroy(Target &t);
	//! A compute pipeline from SPIR-V with `layout`.
	bool
	create_pipeline(const uint32_t *spv, size_t size, VkPipelineLayout layout, VkPipeline *out);

	VkContext &vk_;
	VkCommandPool pool_ = VK_NULL_HANDLE;
	VkCommandBuffer cmd_ = VK_NULL_HANDLE;
	VkFence fence_ = VK_NULL_HANDLE;
	VkSemaphore wait_semaphores_[k_max_blits] = {};
	std::map<uint64_t, Imported> images_;
	uint64_t failures_ = 0;
	uint64_t fence_waits_ = 0;
	VkBuffer sample_buffer_ = VK_NULL_HANDLE;
	VkDeviceMemory sample_memory_ = VK_NULL_HANDLE;
	uint8_t *sample_map_ = nullptr;
	uint8_t samples_[k_samples][4] = {};
	bool pending_ = false; //!< a copy timed out and its fence hasn't signalled yet

	bool composite_ready_ = false, composite_failed_ = false;
	VkSampler sampler_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
	VkDescriptorSet sets_[2] = {};
	Target targets_[2]; //!< per eye, the swapchain's size: what's blitted into the swapchain

	bool upscale_ = false, upscaled_ = false;
	float sharpness_ = 0.2f;
	bool fsr_ready_ = false, fsr_failed_ = false;
	VkPipelineLayout fsr_layout_ = VK_NULL_HANDLE; //!< set_layout_ (sampler + storage image) + 64 bytes
	VkPipeline easu_pipeline_ = VK_NULL_HANDLE, rcas_pipeline_ = VK_NULL_HANDLE;
	VkDescriptorPool fsr_pool_ = VK_NULL_HANDLE;
	VkDescriptorSet easu_sets_[2] = {}, rcas_sets_[2] = {};
	Target fsr_in_[2];  //!< swapchain-sized; the composite (sRGB-encoded) in its top-left region: EASU's input
	Target fsr_mid_[2]; //!< EASU's output at the swapchain's size, RCAS's input
};

} // namespace ml
