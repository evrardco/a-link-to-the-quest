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

	struct Blit
	{
		uint64_t src;          //!< imported texture
		int32_t src_rect[4];   //!< x0, y0, x1, y1 in texels (x1 < x0 or y1 < y0 flips)
		VkImage dst;           //!< swapchain image, in COLOR_ATTACHMENT_OPTIMAL (OpenXR's rule)
		uint32_t dst_width, dst_height;
	};
	//! Record, submit and wait for the blits. False on failure (logged, rate-limited). With `sample`,
	//! also read k_samples texels of the first source (see samples()).
	bool
	copy(const Blit *blits, int count, bool sample = false);

	//! Diagnostics: texels of the first blit's source at its centre and quarter points, RGBA8.
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
		int sync_fd = -1;          //!< a dup of the DMA-BUF, to export its write fences from
		bool initialized = false;  //!< moved out of UNDEFINED (into GENERAL) once
		uint32_t width = 0, height = 0;
	};

	void
	destroy(Imported &img);
	//! A semaphore signalled when the writers of `img` are done, or VK_NULL_HANDLE.
	VkSemaphore
	write_fence_semaphore(Imported &img, int slot);

	VkContext &vk_;
	VkCommandPool pool_ = VK_NULL_HANDLE;
	VkCommandBuffer cmd_ = VK_NULL_HANDLE;
	VkFence fence_ = VK_NULL_HANDLE;
	VkSemaphore wait_semaphores_[4] = {};
	std::map<uint64_t, Imported> images_;
	uint64_t failures_ = 0;
	uint64_t fence_waits_ = 0;
	VkBuffer sample_buffer_ = VK_NULL_HANDLE;
	VkDeviceMemory sample_memory_ = VK_NULL_HANDLE;
	uint8_t *sample_map_ = nullptr;
	uint8_t samples_[k_samples][4] = {};
	bool pending_ = false; //!< a copy timed out and its fence hasn't signalled yet
};

} // namespace ml
