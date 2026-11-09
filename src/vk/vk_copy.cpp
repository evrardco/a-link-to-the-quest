#include "vk/vk_copy.h"

#include "util/log.h"

#include "composite_comp.h" // generated from composite.comp (SPIR-V)
#include "fsr_easu_comp.h"
#include "fsr_rcas_comp.h"
#include "vk/fsr.h"

#include <cmath>

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
	for (int e = 0; e < 2; e++) {
		destroy(targets_[e]);
		destroy(fsr_in_[e]);
		destroy(fsr_mid_[e]);
	}
	if (fsr_pool_ != VK_NULL_HANDLE) {
		vk_.DestroyDescriptorPool(vk_.device, fsr_pool_, nullptr);
	}
	for (VkPipeline p : {easu_pipeline_, rcas_pipeline_}) {
		if (p != VK_NULL_HANDLE) {
			vk_.DestroyPipeline(vk_.device, p, nullptr);
		}
	}
	if (fsr_layout_ != VK_NULL_HANDLE) {
		vk_.DestroyPipelineLayout(vk_.device, fsr_layout_, nullptr);
	}
	if (descriptor_pool_ != VK_NULL_HANDLE) {
		vk_.DestroyDescriptorPool(vk_.device, descriptor_pool_, nullptr); // frees sets_
	}
	if (pipeline_ != VK_NULL_HANDLE) {
		vk_.DestroyPipeline(vk_.device, pipeline_, nullptr);
	}
	if (pipeline_layout_ != VK_NULL_HANDLE) {
		vk_.DestroyPipelineLayout(vk_.device, pipeline_layout_, nullptr);
	}
	if (set_layout_ != VK_NULL_HANDLE) {
		vk_.DestroyDescriptorSetLayout(vk_.device, set_layout_, nullptr);
	}
	if (sampler_ != VK_NULL_HANDLE) {
		vk_.DestroySampler(vk_.device, sampler_, nullptr);
	}
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
	if (img.view != VK_NULL_HANDLE) {
		vk_.DestroyImageView(vk_.device, img.view, nullptr);
	}
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

	// For composite(): sampled through its own (sRGB) format. Optional: the plain copy doesn't need it.
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = img.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = format;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vk_.CreateImageView(vk_.device, &vci, nullptr, &img.view) != VK_SUCCESS) {
		img.view = VK_NULL_HANDLE;
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
VkCopier::copy(const Blit *blits, int count, int sample_blit)
{
	if (count > k_max_blits) {
		count = k_max_blits;
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

	VkSemaphore waits[k_max_blits];
	VkPipelineStageFlags wait_stages[k_max_blits];
	uint32_t wait_count = 0;
	VkImageMemoryBarrier before[2 * k_max_blits], after[k_max_blits];
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

	const bool sample = sample_map_ != nullptr && sample_blit >= 0 && sample_blit < count;
	if (sample) {
		// Diagnostics: what SteamVR rendered (the source is in GENERAL, readable by transfers).
		const Imported &src = images_[blits[sample_blit].src];
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

bool
VkCopier::fail(const char *what)
{
	if (failures_++ % 1000 == 0) {
		log("Vulkan copy: %s (failure #%llu)", what, (unsigned long long)failures_);
	}
	return false;
}

bool
VkCopier::init_composite()
{
	if (composite_ready_ || composite_failed_) {
		return composite_ready_;
	}
	composite_failed_ = true; // until everything below succeeded

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (vk_.CreateSampler(vk_.device, &sci, nullptr, &sampler_) != VK_SUCCESS) {
		log("Vulkan composite: vkCreateSampler failed");
		return false;
	}

	VkDescriptorSetLayoutBinding bindings[2] = {};
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = k_composite_layers;
	bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	dli.bindingCount = 2;
	dli.pBindings = bindings;
	if (vk_.CreateDescriptorSetLayout(vk_.device, &dli, nullptr, &set_layout_) != VK_SUCCESS) {
		log("Vulkan composite: vkCreateDescriptorSetLayout failed");
		return false;
	}

	VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) * 4 * k_composite_layers + 4 * sizeof(int32_t)};
	VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &set_layout_;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	if (vk_.CreatePipelineLayout(vk_.device, &pli, nullptr, &pipeline_layout_) != VK_SUCCESS) {
		log("Vulkan composite: vkCreatePipelineLayout failed");
		return false;
	}

	if (!create_pipeline(ml_composite_comp_spv, sizeof(ml_composite_comp_spv), pipeline_layout_, &pipeline_)) {
		log("Vulkan composite: creating the pipeline failed");
		return false;
	}

	VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * k_composite_layers},
	                                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
	VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 2;
	dpi.poolSizeCount = 2;
	dpi.pPoolSizes = sizes;
	if (vk_.CreateDescriptorPool(vk_.device, &dpi, nullptr, &descriptor_pool_) != VK_SUCCESS) {
		log("Vulkan composite: vkCreateDescriptorPool failed");
		return false;
	}
	VkDescriptorSetLayout layouts[2] = {set_layout_, set_layout_};
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = descriptor_pool_;
	dai.descriptorSetCount = 2;
	dai.pSetLayouts = layouts;
	if (vk_.AllocateDescriptorSets(vk_.device, &dai, sets_) != VK_SUCCESS) {
		log("Vulkan composite: vkAllocateDescriptorSets failed");
		return false;
	}

	composite_failed_ = false;
	composite_ready_ = true;
	log("Vulkan: layer compositing ready");
	return true;
}

void
VkCopier::destroy(Target &t)
{
	if (t.view != VK_NULL_HANDLE) {
		vk_.DestroyImageView(vk_.device, t.view, nullptr);
	}
	if (t.image != VK_NULL_HANDLE) {
		vk_.DestroyImage(vk_.device, t.image, nullptr);
	}
	if (t.memory != VK_NULL_HANDLE) {
		vk_.FreeMemory(vk_.device, t.memory, nullptr);
	}
	t = Target{};
}

bool
VkCopier::ensure_image(Target &t, uint32_t w, uint32_t h, VkImageUsageFlags usage)
{
	if (t.image != VK_NULL_HANDLE && t.width == w && t.height == h) {
		return true;
	}
	destroy(t);

	// Linear float: blending happens in linear light and the blit into the sRGB swapchain encodes
	// without the banding an 8-bit linear image would add. (Storage on sRGB formats isn't allowed.)
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vk_.CreateImage(vk_.device, &ici, nullptr, &t.image) != VK_SUCCESS) {
		t.image = VK_NULL_HANDLE;
		return false;
	}
	VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
	ri.image = t.image;
	VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
	vk_.GetImageMemoryRequirements2(vk_.device, &ri, &req);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.memoryRequirements.size;
	mai.memoryTypeIndex = vk_.memory_type(req.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mai.memoryTypeIndex == UINT32_MAX || vk_.AllocateMemory(vk_.device, &mai, nullptr, &t.memory) != VK_SUCCESS ||
	    vk_.BindImageMemory(vk_.device, t.image, t.memory, 0) != VK_SUCCESS) {
		return false; // freed on the next call or in the destructor
	}
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = t.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = ici.format;
	vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	if (vk_.CreateImageView(vk_.device, &vci, nullptr, &t.view) != VK_SUCCESS) {
		t.view = VK_NULL_HANDLE;
		return false;
	}
	t.width = w;
	t.height = h;
	return true;
}

bool
VkCopier::create_pipeline(const uint32_t *spv, size_t size, VkPipelineLayout layout, VkPipeline *out)
{
	VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	smi.codeSize = size;
	smi.pCode = spv;
	VkShaderModule module = VK_NULL_HANDLE;
	if (vk_.CreateShaderModule(vk_.device, &smi, nullptr, &module) != VK_SUCCESS) {
		return false;
	}
	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
	cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	cpi.stage.module = module;
	cpi.stage.pName = "main";
	cpi.layout = layout;
	const VkResult r = vk_.CreateComputePipelines(vk_.device, VK_NULL_HANDLE, 1, &cpi, nullptr, out);
	vk_.DestroyShaderModule(vk_.device, module, nullptr);
	if (r != VK_SUCCESS) {
		*out = VK_NULL_HANDLE;
		return false;
	}
	return true;
}

bool
VkCopier::init_fsr()
{
	if (fsr_ready_ || fsr_failed_) {
		return fsr_ready_;
	}
	fsr_failed_ = true; // until everything below succeeded

	// Same set layout as the composite (sampled image(s) at binding 0, storage image at 1): EASU and
	// RCAS only use the first sampler slot, the others get valid dummies.
	VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 * sizeof(uint32_t)};
	VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	pli.setLayoutCount = 1;
	pli.pSetLayouts = &set_layout_;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pcr;
	if (vk_.CreatePipelineLayout(vk_.device, &pli, nullptr, &fsr_layout_) != VK_SUCCESS) {
		log("Vulkan FSR: vkCreatePipelineLayout failed: upscaling bilinearly");
		return false;
	}
	if (!create_pipeline(ml_fsr_easu_comp_spv, sizeof(ml_fsr_easu_comp_spv), fsr_layout_, &easu_pipeline_) ||
	    !create_pipeline(ml_fsr_rcas_comp_spv, sizeof(ml_fsr_rcas_comp_spv), fsr_layout_, &rcas_pipeline_)) {
		log("Vulkan FSR: creating the EASU/RCAS pipelines failed: upscaling bilinearly");
		return false;
	}
	VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * k_composite_layers},
	                                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
	VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	dpi.maxSets = 4;
	dpi.poolSizeCount = 2;
	dpi.pPoolSizes = sizes;
	if (vk_.CreateDescriptorPool(vk_.device, &dpi, nullptr, &fsr_pool_) != VK_SUCCESS) {
		log("Vulkan FSR: vkCreateDescriptorPool failed: upscaling bilinearly");
		return false;
	}
	VkDescriptorSetLayout layouts[4] = {set_layout_, set_layout_, set_layout_, set_layout_};
	VkDescriptorSet sets[4];
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = fsr_pool_;
	dai.descriptorSetCount = 4;
	dai.pSetLayouts = layouts;
	if (vk_.AllocateDescriptorSets(vk_.device, &dai, sets) != VK_SUCCESS) {
		log("Vulkan FSR: vkAllocateDescriptorSets failed: upscaling bilinearly");
		return false;
	}
	easu_sets_[0] = sets[0];
	easu_sets_[1] = sets[1];
	rcas_sets_[0] = sets[2];
	rcas_sets_[1] = sets[3];
	fsr_failed_ = false;
	fsr_ready_ = true;
	log("Vulkan: FSR 1 upscaling ready (sharpness %.2f)", sharpness_);
	return true;
}

bool
VkCopier::composite(const CompositeEye eyes[2])
{
	if (!init_composite()) {
		return false;
	}
	if (pending_) {
		if (vk_.WaitForFences(vk_.device, 1, &fence_, VK_TRUE, 0) != VK_SUCCESS) {
			return fail("the previous copy is still running");
		}
		vk_.ResetFences(vk_.device, 1, &fence_);
		pending_ = false;
	}

	// FSR: the composite runs at the scene's size (its bounds in its texture, at most the swapchain's),
	// EASU scales it up, RCAS sharpens. Every frame while upscaling, even one that's large enough
	// (then a 1:1 pass): games and SteamVR shrink the bounds under load (dynamic resolution, seen
	// 0.869 in HL2:VR), and deciding per frame alternated sharpened and plain frames (2026-10-30).
	// The region varies inside fixed-size images: reallocating them as it changed dropped frames.
	uint32_t in_w[2] = {}, in_h[2] = {};
	bool fsr = upscale_;
	for (int e = 0; e < 2 && fsr; e++) {
		auto it = images_.find(eyes[e].src[0]);
		if (it == images_.end()) {
			break; // reported below
		}
		in_w[e] = (uint32_t)std::lround(std::fabs(eyes[e].rect[0][2] - eyes[e].rect[0][0]) * it->second.width);
		in_h[e] = (uint32_t)std::lround(std::fabs(eyes[e].rect[0][3] - eyes[e].rect[0][1]) * it->second.height);
		in_w[e] = in_w[e] < 1 ? 1 : in_w[e] > eyes[e].dst_width ? eyes[e].dst_width : in_w[e];
		in_h[e] = in_h[e] < 1 ? 1 : in_h[e] > eyes[e].dst_height ? eyes[e].dst_height : in_h[e];
	}
	fsr = fsr && init_fsr();

	// Descriptors first: a set must not change after it's bound in a recorded command buffer. The GPU
	// is idle for our work here (every copy waits for its fence).
	VkDescriptorImageInfo sampled[2][k_composite_layers];
	VkDescriptorImageInfo storage[2];
	VkDescriptorImageInfo fsr_sampled[2][2][k_composite_layers]; // [eye][easu, rcas][slot]
	VkDescriptorImageInfo fsr_storage[2][2];
	VkWriteDescriptorSet writes[12];
	uint32_t nw = 0;
	auto write = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType type, uint32_t count,
	                 const VkDescriptorImageInfo *info) {
		VkWriteDescriptorSet &w = writes[nw++];
		w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		w.dstSet = set;
		w.dstBinding = binding;
		w.descriptorCount = count;
		w.descriptorType = type;
		w.pImageInfo = info;
	};
	for (int e = 0; e < 2; e++) {
		const CompositeEye &eye = eyes[e];
		if (eye.count < 1 || eye.count > k_composite_layers) {
			return fail("composite: bad layer count");
		}
		for (int l = 0; l < k_composite_layers; l++) {
			// Unused slots repeat the scene: every descriptor must be valid.
			auto it = images_.find(eye.src[l < eye.count ? l : 0]);
			if (it == images_.end() || it->second.view == VK_NULL_HANDLE) {
				return fail("composite: source texture not imported (or no view)");
			}
			sampled[e][l] = {sampler_, it->second.view, VK_IMAGE_LAYOUT_GENERAL};
		}
		if (!ensure_image(targets_[e], eye.dst_width, eye.dst_height,
		                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
			return fail("composite: can't create the intermediate image");
		}
		if (fsr && (!ensure_image(fsr_in_[e], eye.dst_width, eye.dst_height,
		                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
		            !ensure_image(fsr_mid_[e], eye.dst_width, eye.dst_height,
		                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))) {
			return fail("composite: can't create FSR's images");
		}
		storage[e] = {VK_NULL_HANDLE, fsr ? fsr_in_[e].view : targets_[e].view, VK_IMAGE_LAYOUT_GENERAL};
		write(sets_[e], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_composite_layers, sampled[e]);
		write(sets_[e], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, &storage[e]);
		if (fsr) {
			const Target *in[2] = {&fsr_in_[e], &fsr_mid_[e]}, *out[2] = {&fsr_mid_[e], &targets_[e]};
			const VkDescriptorSet sets[2] = {easu_sets_[e], rcas_sets_[e]};
			for (int pass = 0; pass < 2; pass++) {
				for (int l = 0; l < k_composite_layers; l++) {
					fsr_sampled[e][pass][l] = {sampler_, in[pass]->view, VK_IMAGE_LAYOUT_GENERAL};
				}
				fsr_storage[e][pass] = {VK_NULL_HANDLE, out[pass]->view, VK_IMAGE_LAYOUT_GENERAL};
				write(sets[pass], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_composite_layers, fsr_sampled[e][pass]);
				write(sets[pass], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, &fsr_storage[e][pass]);
			}
		}
	}
	vk_.UpdateDescriptorSets(vk_.device, nw, writes, 0, nullptr);

	vk_.ResetCommandBuffer(cmd_, 0);
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vk_.BeginCommandBuffer(cmd_, &bi);
	const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	auto barrier = [&](VkImage image, VkAccessFlags src, VkAccessFlags dst, VkImageLayout from, VkImageLayout to) {
		VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		b.srcAccessMask = src;
		b.dstAccessMask = dst;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = range;
		return b;
	};

	// Sources: wait for SteamVR's rendering (DMA-BUF fences) and make them readable, each once.
	VkSemaphore waits[k_max_blits];
	VkPipelineStageFlags wait_stages[k_max_blits];
	uint32_t wait_count = 0;
	VkImageMemoryBarrier before[k_max_blits + 6];
	uint32_t nb = 0;
	uint64_t seen[k_max_blits];
	int nseen = 0;
	for (int e = 0; e < 2; e++) {
		for (int l = 0; l < eyes[e].count; l++) {
			const uint64_t handle = eyes[e].src[l];
			bool dup = false;
			for (int i = 0; i < nseen; i++) {
				dup |= seen[i] == handle;
			}
			if (dup || nseen >= k_max_blits) {
				continue;
			}
			Imported &src = images_[handle];
			if (VkSemaphore s = write_fence_semaphore(src, nseen); s != VK_NULL_HANDLE) {
				waits[wait_count] = s;
				wait_stages[wait_count++] = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
				fence_waits_++;
			}
			seen[nseen++] = handle;
			// Same layout rule as copy(): out of UNDEFINED once, then GENERAL for good.
			before[nb++] = barrier(src.image, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			                       src.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
			                       VK_IMAGE_LAYOUT_GENERAL);
			src.initialized = true;
		}
	}
	// Our own images are overwritten entirely: from UNDEFINED, after their last use (the final blit
	// read targets_, the FSR passes read the others).
	for (int e = 0; e < 2; e++) {
		before[nb++] = barrier(targets_[e].image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		if (fsr) {
			before[nb++] = barrier(fsr_in_[e].image, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
			before[nb++] = barrier(fsr_mid_[e].image, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		}
	}
	vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
	                       nullptr, 0, nullptr, nb, before);

	vk_.CmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
	for (int e = 0; e < 2; e++) {
		struct
		{
			float rect[k_composite_layers][4];
			int32_t count;
			int32_t encode;
			int32_t size[2];
		} params{};
		memcpy(params.rect, eyes[e].rect, sizeof(params.rect));
		params.count = eyes[e].count;
		params.encode = fsr ? 1 : 0;
		params.size[0] = fsr ? (int32_t)in_w[e] : 0;
		params.size[1] = fsr ? (int32_t)in_h[e] : 0;
		// FSR: the region plus the repeated edge row and column (composite.comp).
		const uint32_t w = fsr ? in_w[e] + 1 : eyes[e].dst_width, h = fsr ? in_h[e] + 1 : eyes[e].dst_height;
		vk_.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1, &sets_[e], 0, nullptr);
		vk_.CmdPushConstants(cmd_, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
		vk_.CmdDispatch(cmd_, (w + 7) / 8, (h + 7) / 8, 1);
	}

	if (fsr) {
		// composite -> EASU -> RCAS, each reading what the previous pass wrote.
		const VkPipeline pipelines[2] = {easu_pipeline_, rcas_pipeline_};
		for (int pass = 0; pass < 2; pass++) {
			VkImageMemoryBarrier rb[2];
			for (int e = 0; e < 2; e++) {
				rb[e] = barrier(pass == 0 ? fsr_in_[e].image : fsr_mid_[e].image, VK_ACCESS_SHADER_WRITE_BIT,
				                VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
			}
			vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
			                       nullptr, 0, nullptr, 2, rb);
			vk_.CmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pass]);
			for (int e = 0; e < 2; e++) {
				uint32_t con[16] = {};
				if (pass == 0) {
					fsr_easu_constants(con, (float)in_w[e], (float)in_h[e], (float)fsr_in_[e].width,
					                   (float)fsr_in_[e].height, (float)eyes[e].dst_width, (float)eyes[e].dst_height);
				} else {
					fsr_rcas_constants(con, sharpness_);
				}
				vk_.CmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, fsr_layout_, 0, 1,
				                          pass == 0 ? &easu_sets_[e] : &rcas_sets_[e], 0, nullptr);
				vk_.CmdPushConstants(cmd_, fsr_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(con), con);
				// 16x16 pixels per 64-thread group (fsr_*.comp).
				vk_.CmdDispatch(cmd_, (eyes[e].dst_width + 15) / 16, (eyes[e].dst_height + 15) / 16, 1);
			}
		}
	}
	upscaled_ = fsr;

	// Intermediate -> swapchain (same size: the blit only converts linear float to sRGB).
	VkImageMemoryBarrier mid[4];
	for (int e = 0; e < 2; e++) {
		mid[2 * e] = barrier(targets_[e].image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
		                     VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		mid[2 * e + 1] = barrier(eyes[e].dst, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	}
	vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 4, mid);
	VkImageMemoryBarrier after[2];
	for (int e = 0; e < 2; e++) {
		VkImageBlit blit{};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blit.srcOffsets[1] = {(int32_t)eyes[e].dst_width, (int32_t)eyes[e].dst_height, 1};
		blit.dstSubresource = blit.srcSubresource;
		blit.dstOffsets[1] = blit.srcOffsets[1];
		vk_.CmdBlitImage(cmd_, targets_[e].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, eyes[e].dst,
		                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
		after[e] = barrier(eyes[e].dst, VK_ACCESS_TRANSFER_WRITE_BIT,
		                   VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
		                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	}
	vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
	                       nullptr, 2, after);
	if (vk_.EndCommandBuffer(cmd_) != VK_SUCCESS) {
		return fail("composite: vkEndCommandBuffer failed");
	}

	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.waitSemaphoreCount = wait_count;
	si.pWaitSemaphores = waits;
	si.pWaitDstStageMask = wait_stages;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd_;
	if (vk_.QueueSubmit(vk_.queue, 1, &si, fence_) != VK_SUCCESS) {
		return fail("composite: vkQueueSubmit failed");
	}
	VkResult r = vk_.WaitForFences(vk_.device, 1, &fence_, VK_TRUE, k_copy_timeout_ns);
	if (r != VK_SUCCESS) {
		pending_ = true;
		return fail(r == VK_TIMEOUT ? "composite: GPU timed out" : "composite: vkWaitForFences failed");
	}
	vk_.ResetFences(vk_.device, 1, &fence_);
	return true;
}

} // namespace ml
