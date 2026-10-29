#include "vk/vk_copy.h"

#include "util/log.h"

#include <cstring>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ml {

//! Longest we wait for our copy: a frame is ~8 ms, so this only trips on a hung GPU.
static constexpr uint64_t k_copy_timeout_ns = 500'000'000;

VkCopier::~VkCopier()
{
	if (vk_.device == VK_NULL_HANDLE) {
		return;
	}
	if (fence_ != VK_NULL_HANDLE) {
		// A copy that timed out may still be running; never free what it uses.
		vk_.WaitForFences(vk_.device, 1, &fence_, VK_TRUE, k_copy_timeout_ns);
	}
	for (auto &[handle, img] : images_) {
		destroy(img);
	}
	images_.clear();
	for (auto &s : wait_semaphores_) {
		if (s != VK_NULL_HANDLE) {
			vk_.DestroySemaphore(vk_.device, s, nullptr);
		}
	}
	if (fence_ != VK_NULL_HANDLE) {
		vk_.DestroyFence(vk_.device, fence_, nullptr);
	}
	if (sample_buffer_ != VK_NULL_HANDLE) {
		vk_.DestroyBuffer(vk_.device, sample_buffer_, nullptr);
	}
	if (sample_memory_ != VK_NULL_HANDLE) {
		vk_.FreeMemory(vk_.device, sample_memory_, nullptr); // unmaps
	}
	if (pool_ != VK_NULL_HANDLE) {
		vk_.DestroyCommandPool(vk_.device, pool_, nullptr); // frees cmd_
	}
}

bool
VkCopier::init()
{
	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = vk_.queue_family;
	if (vk_.CreateCommandPool(vk_.device, &pci, nullptr, &pool_) != VK_SUCCESS) {
		log("Vulkan: vkCreateCommandPool failed");
		return false;
	}
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool_;
	cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cai.commandBufferCount = 1;
	if (vk_.AllocateCommandBuffers(vk_.device, &cai, &cmd_) != VK_SUCCESS) {
		log("Vulkan: vkAllocateCommandBuffers failed");
		return false;
	}
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	if (vk_.CreateFence(vk_.device, &fci, nullptr, &fence_) != VK_SUCCESS) {
		log("Vulkan: vkCreateFence failed");
		return false;
	}
	if (vk_.has_semaphore_fd) {
		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		for (auto &s : wait_semaphores_) {
			vk_.CreateSemaphore(vk_.device, &sci, nullptr, &s);
		}
	}

	// A small host-visible buffer for the diagnostic texel samples (optional: failure isn't fatal).
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = sizeof(samples_);
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	if (vk_.CreateBuffer(vk_.device, &bci, nullptr, &sample_buffer_) == VK_SUCCESS) {
		VkMemoryRequirements req;
		vk_.GetBufferMemoryRequirements(vk_.device, sample_buffer_, &req);
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = vk_.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		void *map = nullptr;
		if (mai.memoryTypeIndex != UINT32_MAX &&
		    vk_.AllocateMemory(vk_.device, &mai, nullptr, &sample_memory_) == VK_SUCCESS &&
		    vk_.BindBufferMemory(vk_.device, sample_buffer_, sample_memory_, 0) == VK_SUCCESS &&
		    vk_.MapMemory(vk_.device, sample_memory_, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS) {
			sample_map_ = (uint8_t *)map;
		}
	}
	return true;
}

void
VkCopier::destroy(Imported &img)
{
	if (img.image != VK_NULL_HANDLE) {
		vk_.DestroyImage(vk_.device, img.image, nullptr);
	}
	if (img.memory != VK_NULL_HANDLE) {
		vk_.FreeMemory(vk_.device, img.memory, nullptr);
	}
	if (img.sync_fd >= 0) {
		close(img.sync_fd);
	}
	img = Imported{};
}

bool
VkCopier::import(uint64_t handle, int fd, VkFormat format, uint32_t width, uint32_t height)
{
	if (fd < 0) {
		return false;
	}
	Imported img;
	img.width = width;
	img.height = height;
	img.sync_fd = dup(fd); // Vulkan consumes `fd` on a successful import

	VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = &emi;
	ici.flags = k_shared_image_flags;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = format;
	ici.extent = {width, height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = k_shared_image_usage;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vk_.CreateImage(vk_.device, &ici, nullptr, &img.image) != VK_SUCCESS) {
		log("Vulkan: vkCreateImage for SteamVR texture %llu failed", (unsigned long long)handle);
		close(fd);
		destroy(img);
		return false;
	}

	VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
	ri.image = img.image;
	VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
	vk_.GetImageMemoryRequirements2(vk_.device, &ri, &req);

	VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	imp.fd = fd;
	VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.pNext = &imp;
	ded.image = img.image;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &ded;
	mai.allocationSize = req.memoryRequirements.size;
	mai.memoryTypeIndex = vk_.memory_type(req.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mai.memoryTypeIndex == UINT32_MAX ||
	    vk_.AllocateMemory(vk_.device, &mai, nullptr, &img.memory) != VK_SUCCESS) {
		log("Vulkan: importing SteamVR texture %llu failed (memory type %u)", (unsigned long long)handle,
		    mai.memoryTypeIndex);
		close(fd); // not consumed on failure
		destroy(img);
		return false;
	}
	if (vk_.BindImageMemory(vk_.device, img.image, img.memory, 0) != VK_SUCCESS) {
		log("Vulkan: vkBindImageMemory for SteamVR texture %llu failed", (unsigned long long)handle);
		destroy(img);
		return false;
	}

	log("Vulkan: imported SteamVR texture %llu (VkFormat %d, %ux%u, %llu bytes)", (unsigned long long)handle,
	    (int)format, width, height, (unsigned long long)req.memoryRequirements.size);
	images_[handle] = img;
	return true;
}

void
VkCopier::forget(uint64_t handle)
{
	auto it = images_.find(handle);
	if (it != images_.end()) {
		destroy(it->second);
		images_.erase(it);
	}
}

VkSemaphore
VkCopier::write_fence_semaphore(Imported &img, int slot)
{
	if (!vk_.has_semaphore_fd || img.sync_fd < 0 || wait_semaphores_[slot] == VK_NULL_HANDLE) {
		return VK_NULL_HANDLE;
	}
	// The fences a reader must wait for: SteamVR's compositor's rendering into this texture.
	dma_buf_export_sync_file exp{};
	exp.flags = DMA_BUF_SYNC_READ;
	exp.fd = -1;
	if (ioctl(img.sync_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exp) != 0 || exp.fd < 0) {
		return VK_NULL_HANDLE; // old kernel, or not a DMA-BUF
	}
	VkImportSemaphoreFdInfoKHR isi{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
	isi.semaphore = wait_semaphores_[slot];
	isi.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
	isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	isi.fd = exp.fd;
	if (vk_.ImportSemaphoreFdKHR(vk_.device, &isi) != VK_SUCCESS) {
		close(exp.fd);
		return VK_NULL_HANDLE;
	}
	return wait_semaphores_[slot]; // the import took the fd
}

bool
VkCopier::copy(const Blit *blits, int count, bool sample)
{
	if (count > 4) {
		count = 4;
	}
	auto fail = [&](const char *what) {
		if (failures_++ % 1000 == 0) {
			log("Vulkan copy: %s (failure #%llu)", what, (unsigned long long)failures_);
		}
		return false;
	};

	if (pending_) {
		// The last copy timed out: its command buffer may still run, and must not be re-recorded.
		if (vk_.WaitForFences(vk_.device, 1, &fence_, VK_TRUE, 0) != VK_SUCCESS) {
			return fail("the previous copy is still running");
		}
		vk_.ResetFences(vk_.device, 1, &fence_);
		pending_ = false;
	}

	vk_.ResetCommandBuffer(cmd_, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vk_.BeginCommandBuffer(cmd_, &bi);

	VkSemaphore waits[4];
	VkPipelineStageFlags wait_stages[4];
	uint32_t wait_count = 0;
	VkImageMemoryBarrier before[8], after[4];
	uint32_t nb = 0, na = 0;
	const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

	for (int i = 0; i < count; i++) {
		auto it = images_.find(blits[i].src);
		if (it == images_.end()) {
			vk_.EndCommandBuffer(cmd_);
			return fail("source texture not imported");
		}
		Imported &src = it->second;

		// Same texture for both eyes (one set): one barrier and one fence wait are enough.
		bool seen = false;
		for (int j = 0; j < i; j++) {
			seen |= blits[j].src == blits[i].src;
		}
		if (!seen) {
			if (VkSemaphore s = write_fence_semaphore(src, i); s != VK_NULL_HANDLE) {
				waits[wait_count] = s;
				wait_stages[wait_count++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
				fence_waits_++;
			}
			// Like ALVR: leave UNDEFINED once (into GENERAL), then stay in GENERAL. A per-frame
			// transition from UNDEFINED would let the driver discard (or, with DCC, reinitialise)
			// what SteamVR rendered.
			VkImageMemoryBarrier &b = before[nb++];
			b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			b.oldLayout = src.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = src.image;
			b.subresourceRange = range;
			src.initialized = true;
		}

		VkImageMemoryBarrier &d = before[nb++];
		d = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		d.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		d.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		d.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		d.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		d.srcQueueFamilyIndex = d.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		d.image = blits[i].dst;
		d.subresourceRange = range;

		VkImageMemoryBarrier &a = after[na++];
		a = d;
		a.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		a.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
		a.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		a.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
	                       nullptr, nb, before);
	for (int i = 0; i < count; i++) {
		const Blit &b = blits[i];
		VkImageBlit region{};
		region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.srcOffsets[0] = {b.src_rect[0], b.src_rect[1], 0};
		region.srcOffsets[1] = {b.src_rect[2], b.src_rect[3], 1};
		region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.dstOffsets[0] = {0, 0, 0};
		region.dstOffsets[1] = {(int32_t)b.dst_width, (int32_t)b.dst_height, 1};
		vk_.CmdBlitImage(cmd_, images_[b.src].image, VK_IMAGE_LAYOUT_GENERAL, b.dst,
		                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
	}
	vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
	                       nullptr, na, after);

	sample = sample && sample_map_ != nullptr && count > 0;
	if (sample) {
		// Diagnostics: what SteamVR rendered (the source is in GENERAL, readable by transfers).
		const Imported &src = images_[blits[0].src];
		const float at[k_samples][2] = {{0.5f, 0.5f}, {0.25f, 0.25f}, {0.75f, 0.25f}, {0.25f, 0.75f}, {0.75f, 0.75f}};
		VkBufferImageCopy regions[k_samples];
		for (int i = 0; i < k_samples; i++) {
			regions[i] = {};
			regions[i].bufferOffset = (VkDeviceSize)i * 4;
			regions[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			regions[i].imageOffset = {(int32_t)(at[i][0] * (src.width - 1)), (int32_t)(at[i][1] * (src.height - 1)), 0};
			regions[i].imageExtent = {1, 1, 1};
		}
		vk_.CmdCopyImageToBuffer(cmd_, src.image, VK_IMAGE_LAYOUT_GENERAL, sample_buffer_, k_samples, regions);
		VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
		mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr,
		                       0, nullptr);
	}
	if (vk_.EndCommandBuffer(cmd_) != VK_SUCCESS) {
		return fail("vkEndCommandBuffer failed");
	}

	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = wait_count;
	si.pWaitSemaphores = waits;
	si.pWaitDstStageMask = wait_stages;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd_;
	if (vk_.QueueSubmit(vk_.queue, 1, &si, fence_) != VK_SUCCESS) {
		return fail("vkQueueSubmit failed");
	}
	VkResult r = vk_.WaitForFences(vk_.device, 1, &fence_, VK_TRUE, k_copy_timeout_ns);
	if (r != VK_SUCCESS) {
		// The fence stays pending: the next copy and the destructor check it again first.
		pending_ = true;
		return fail(r == VK_TIMEOUT ? "GPU copy timed out" : "vkWaitForFences failed");
	}
	vk_.ResetFences(vk_.device, 1, &fence_);
	if (sample) {
		memcpy(samples_, sample_map_, sizeof(samples_));
	}
	return true;
}

} // namespace ml
