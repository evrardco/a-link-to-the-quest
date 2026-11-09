// GPU self-test of the frame copy path, without SteamVR or Monado: makes an image the way SteamVR
// does (exportable as an opaque fd), fills it with a known colour, imports it with VkCopier and blits
// it into a "swapchain" image exactly as the driver does, then reads the pixels back.
// Usage: monadolink_vk_selftest     (exit code 0 = all checks passed)
#include "vk/vk_copy.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

using namespace ml;

static VkContext vk;
static PFN_vkGetMemoryFdKHR GetMemoryFdKHR;
static PFN_vkCmdClearColorImage CmdClearColorImage;
static PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
static PFN_vkCreateBuffer CreateBuffer;
static PFN_vkDestroyBuffer DestroyBuffer;
static PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
static PFN_vkBindBufferMemory BindBufferMemory;
static PFN_vkMapMemory MapMemory;
static VkCommandPool pool;
static VkCommandBuffer cmd;
static VkFence fence;
static int failures = 0;

#define CHECK_VK(call)                                                                                                 \
	do {                                                                                                           \
		VkResult r_ = (call);                                                                                  \
		if (r_ != VK_SUCCESS) {                                                                                \
			fprintf(stderr, "FAIL: %s = %d\n", #call, (int)r_);                                            \
			exit(1);                                                                                       \
		}                                                                                                      \
	} while (0)

static void
expect(bool ok, const char *what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	failures += ok ? 0 : 1;
}

//! Record with `fn`, submit, wait.
template <typename F>
static void
run(F fn)
{
	VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	CHECK_VK(vk.ResetCommandBuffer(cmd, 0));
	CHECK_VK(vk.BeginCommandBuffer(cmd, &bi));
	fn();
	CHECK_VK(vk.EndCommandBuffer(cmd));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	CHECK_VK(vk.QueueSubmit(vk.queue, 1, &si, fence));
	CHECK_VK(vk.WaitForFences(vk.device, 1, &fence, VK_TRUE, 2'000'000'000));
	CHECK_VK(vk.ResetFences(vk.device, 1, &fence));
}

static void
barrier(VkImage image, VkImageLayout from, VkImageLayout to)
{
	VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
	                      0, nullptr, 1, &b);
}

static VkDeviceMemory
bind_memory(VkImage image, bool exportable)
{
	VkImageMemoryRequirementsInfo2 ri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
	ri.image = image;
	VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
	vk.GetImageMemoryRequirements2(vk.device, &ri, &req);
	VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.pNext = exportable ? &exp : nullptr;
	ded.image = image;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &ded;
	mai.allocationSize = req.memoryRequirements.size;
	mai.memoryTypeIndex = vk.memory_type(req.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory mem;
	CHECK_VK(vk.AllocateMemory(vk.device, &mai, nullptr, &mem));
	CHECK_VK(vk.BindImageMemory(vk.device, image, mem, 0));
	return mem;
}

//! Read a pixel (RGBA8) of `image` (in COLOR_ATTACHMENT_OPTIMAL, left there).
static void
read_pixels(VkImage image, uint32_t w, uint32_t h, std::vector<uint8_t> *out)
{
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = (VkDeviceSize)w * h * 4;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer buf;
	CHECK_VK(CreateBuffer(vk.device, &bci, nullptr, &buf));
	VkMemoryRequirements req;
	GetBufferMemoryRequirements(vk.device, buf, &req);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = vk.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory mem;
	CHECK_VK(vk.AllocateMemory(vk.device, &mai, nullptr, &mem));
	CHECK_VK(BindBufferMemory(vk.device, buf, mem, 0));
	run([&] {
		barrier(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {w, h, 1};
		CmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
		barrier(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	});
	void *p = nullptr;
	CHECK_VK(MapMemory(vk.device, mem, 0, bci.size, 0, &p));
	out->assign((uint8_t *)p, (uint8_t *)p + bci.size);
	DestroyBuffer(vk.device, buf, nullptr);
	vk.FreeMemory(vk.device, mem, nullptr);
}

//! Like pixel_is, within `tol` per channel (FSR's half-float passes and sRGB round trip round by 1).
static bool
pixel_near(const std::vector<uint8_t> &px, uint32_t w, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, int tol)
{
	const uint8_t *p = &px[((size_t)y * w + x) * 4];
	if (std::abs(p[0] - r) > tol || std::abs(p[1] - g) > tol || std::abs(p[2] - b) > tol) {
		printf("      pixel (%u,%u) = %u,%u,%u, want %u,%u,%u (+-%d)\n", x, y, p[0], p[1], p[2], r, g, b, tol);
		return false;
	}
	return true;
}

static bool
pixel_is(const std::vector<uint8_t> &px, uint32_t w, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b)
{
	const uint8_t *p = &px[((size_t)y * w + x) * 4];
	if (p[0] != r || p[1] != g || p[2] != b) {
		printf("      pixel (%u,%u) = %u,%u,%u, want %u,%u,%u\n", x, y, p[0], p[1], p[2], r, g, b);
		return false;
	}
	return true;
}

int
main()
{
	if (!vk.load_library()) {
		return 1;
	}
	// --- Instance, GPU, device (the driver gets these from OpenXR) -------------------------------------
	auto CreateInstance = (PFN_vkCreateInstance)vk.GetInstanceProcAddr(nullptr, "vkCreateInstance");
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	CHECK_VK(CreateInstance(&ici, nullptr, &vk.instance));
	vk.load_instance_functions();
	auto EnumeratePhysicalDevices =
	    (PFN_vkEnumeratePhysicalDevices)vk.GetInstanceProcAddr(vk.instance, "vkEnumeratePhysicalDevices");
	auto CreateDevice = (PFN_vkCreateDevice)vk.GetInstanceProcAddr(vk.instance, "vkCreateDevice");
	uint32_t n = 0;
	EnumeratePhysicalDevices(vk.instance, &n, nullptr);
	std::vector<VkPhysicalDevice> gpus(n);
	EnumeratePhysicalDevices(vk.instance, &n, gpus.data());
	VkPhysicalDeviceProperties props{};
	for (auto g : gpus) {
		vk.GetPhysicalDeviceProperties(g, &props);
		if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
			vk.physical_device = g;
			break;
		}
	}
	if (vk.physical_device == VK_NULL_HANDLE) {
		fprintf(stderr, "no GPU\n");
		return 1;
	}
	printf("GPU: %s\n", props.deviceName);

	uint32_t fc = 0;
	vk.GetPhysicalDeviceQueueFamilyProperties(vk.physical_device, &fc, nullptr);
	std::vector<VkQueueFamilyProperties> families(fc);
	vk.GetPhysicalDeviceQueueFamilyProperties(vk.physical_device, &fc, families.data());
	for (uint32_t i = 0; i < fc; i++) {
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			vk.queue_family = i;
			break;
		}
	}
	const char *exts[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
	const float prio = 1.f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = vk.queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 2;
	dci.ppEnabledExtensionNames = exts;
	CHECK_VK(CreateDevice(vk.physical_device, &dci, nullptr, &vk.device));
	vk.has_semaphore_fd = true;
	if (!vk.load_device_functions()) {
		return 1;
	}
#define LOAD(name) name = (PFN_vk##name)vk.GetDeviceProcAddr(vk.device, "vk" #name)
	LOAD(GetMemoryFdKHR);
	LOAD(CmdClearColorImage);
	LOAD(CmdCopyImageToBuffer);
	LOAD(CreateBuffer);
	LOAD(DestroyBuffer);
	LOAD(GetBufferMemoryRequirements);
	LOAD(BindBufferMemory);
	LOAD(MapMemory);
#undef LOAD

	VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pci.queueFamilyIndex = vk.queue_family;
	CHECK_VK(vk.CreateCommandPool(vk.device, &pci, nullptr, &pool));
	VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cai.commandPool = pool;
	cai.commandBufferCount = 1;
	CHECK_VK(vk.AllocateCommandBuffers(vk.device, &cai, &cmd));
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CHECK_VK(vk.CreateFence(vk.device, &fci, nullptr, &fence));

	// --- "SteamVR's" texture: exportable, the flags/usage we ask SteamVR for + what rendering adds ----
	const uint32_t sw = 256, sh = 128, dw = 128, dh = 64;
	VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkImageCreateInfo src_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	src_ci.pNext = &emi;
	src_ci.flags = k_shared_image_flags;
	src_ci.imageType = VK_IMAGE_TYPE_2D;
	src_ci.format = VK_FORMAT_R8G8B8A8_SRGB;
	src_ci.extent = {sw, sh, 1};
	src_ci.mipLevels = src_ci.arrayLayers = 1;
	src_ci.samples = VK_SAMPLE_COUNT_1_BIT;
	src_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	src_ci.usage = k_shared_image_usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkImage src;
	CHECK_VK(vk.CreateImage(vk.device, &src_ci, nullptr, &src));
	VkDeviceMemory src_mem = bind_memory(src, true);

	auto fill = [&](float r, float g, float b, VkImageLayout from) {
		run([&] {
			barrier(src, from, VK_IMAGE_LAYOUT_GENERAL);
			VkClearColorValue c{};
			c.float32[0] = r;
			c.float32[1] = g;
			c.float32[2] = b;
			c.float32[3] = 1.f;
			VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			CmdClearColorImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
		});
	};
	fill(1.f, 0.f, 1.f, VK_IMAGE_LAYOUT_UNDEFINED); // magenta

	VkMemoryGetFdInfoKHR gfi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gfi.memory = src_mem;
	gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	int fd = -1;
	CHECK_VK(GetMemoryFdKHR(vk.device, &gfi, &fd));

	// --- The "swapchain" image, in COLOR_ATTACHMENT_OPTIMAL as OpenXR hands it out ------------------
	VkImageCreateInfo dst_ci = src_ci;
	dst_ci.pNext = nullptr;
	dst_ci.flags = 0;
	dst_ci.extent = {dw, dh, 1};
	dst_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	VkImage dst;
	CHECK_VK(vk.CreateImage(vk.device, &dst_ci, nullptr, &dst));
	VkDeviceMemory dst_mem = bind_memory(dst, false);
	run([&] { barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); });

	// --- The driver's path -----------------------------------------------------------------------------
	std::vector<uint8_t> px;
	{
		VkCopier copier(vk);
		expect(copier.init(), "copier init");
		expect(copier.import(42, fd, VK_FORMAT_R8G8B8A8_SRGB, sw, sh), "import the opaque fd");

		VkCopier::Blit blit{42, {0, 0, (int32_t)sw, (int32_t)sh}, dst, dw, dh};
		expect(copier.copy(&blit, 1), "copy (scaled 256x128 -> 128x64)");
		read_pixels(dst, dw, dh, &px);
		expect(pixel_is(px, dw, 0, 0, 255, 0, 255) && pixel_is(px, dw, dw - 1, dh - 1, 255, 0, 255),
		       "the first frame arrives (magenta)");

		// A new frame rendered into the texture (in GENERAL, as SteamVR leaves it): the copy must see
		// it, i.e. no discard by an UNDEFINED transition after the first frame.
		fill(0.f, 1.f, 0.f, VK_IMAGE_LAYOUT_GENERAL); // green
		expect(copier.copy(&blit, 1), "copy the next frame");
		read_pixels(dst, dw, dh, &px);
		expect(pixel_is(px, dw, dw / 2, dh / 2, 0, 255, 0), "the next frame arrives (green), not stale");

		// A sub-rectangle and a vertical flip: bounds handling.
		fill(0.f, 0.f, 1.f, VK_IMAGE_LAYOUT_GENERAL); // blue
		VkCopier::Blit sub{42, {(int32_t)sw / 2, (int32_t)sh, (int32_t)sw, 0}, dst, dw, dh};
		expect(copier.copy(&sub, 1), "copy a flipped half");
		read_pixels(dst, dw, dh, &px);
		expect(pixel_is(px, dw, 3, 3, 0, 0, 255), "flipped half arrives (blue)");

		// The diagnostic texel samples read the source (blue now).
		expect(copier.copy(&blit, 1, 0), "copy with texel samples");
		const auto &smp = copier.samples();
		expect(smp[0][0] == 0 && smp[0][1] == 0 && smp[0][2] == 255 && smp[4][2] == 255, "samples read the source");

		// Two eyes from one texture in one submit (both eyes sharing a set).
		VkCopier::Blit two[2] = {blit, blit};
		expect(copier.copy(two, 2), "copy both eyes from the same texture");

		// M3: the most blits per copy (4 layers x 2 eyes), each into its own image like swapchains.
		{
			VkImage dsts[VkCopier::k_max_blits];
			VkDeviceMemory mems[VkCopier::k_max_blits];
			VkCopier::Blit many[VkCopier::k_max_blits];
			for (int i = 0; i < VkCopier::k_max_blits; i++) {
				CHECK_VK(vk.CreateImage(vk.device, &dst_ci, nullptr, &dsts[i]));
				mems[i] = bind_memory(dsts[i], false);
				run([&] { barrier(dsts[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); });
				many[i] = blit;
				many[i].dst = dsts[i];
			}
			expect(copier.copy(many, VkCopier::k_max_blits), "copy the most blits at once (4 layers x 2 eyes)");
			bool all_blue = true;
			for (int i = 0; i < VkCopier::k_max_blits; i++) {
				read_pixels(dsts[i], dw, dh, &px);
				all_blue = all_blue && pixel_is(px, dw, 3, 3, 0, 0, 255);
			}
			expect(all_blue, "every destination got the picture");
			for (int i = 0; i < VkCopier::k_max_blits; i++) {
				vk.DestroyImage(vk.device, dsts[i], nullptr);
				vk.FreeMemory(vk.device, mems[i], nullptr);
			}
		}

		// M3: compositing an overlay (a second imported texture) over the scene (blue now).
		{
			VkImage ov;
			CHECK_VK(vk.CreateImage(vk.device, &src_ci, nullptr, &ov));
			VkDeviceMemory ov_mem = bind_memory(ov, true);
			VkImageLayout ov_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			auto fill_ov = [&](float r, float g, float b, float a) {
				run([&] {
					barrier(ov, ov_layout, VK_IMAGE_LAYOUT_GENERAL);
					VkClearColorValue c{};
					c.float32[0] = r;
					c.float32[1] = g;
					c.float32[2] = b;
					c.float32[3] = a;
					VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
					CmdClearColorImage(cmd, ov, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
				});
				ov_layout = VK_IMAGE_LAYOUT_GENERAL;
			};
			fill_ov(1.f, 0.f, 0.f, 0.f); // red, fully transparent
			VkMemoryGetFdInfoKHR ofi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
			ofi.memory = ov_mem;
			ofi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
			int ofd = -1;
			CHECK_VK(GetMemoryFdKHR(vk.device, &ofi, &ofd));
			expect(copier.import(43, ofd, VK_FORMAT_R8G8B8A8_SRGB, sw, sh), "import the overlay texture");

			VkCopier::CompositeEye eyes[2] = {};
			for (auto &e : eyes) {
				e.count = 2;
				e.src[0] = 42;
				e.src[1] = 43;
				for (int l = 0; l < 2; l++) {
					e.rect[l][0] = 0.f;
					e.rect[l][1] = 0.f;
					e.rect[l][2] = 1.f;
					e.rect[l][3] = 1.f;
				}
				e.dst = dst;
				e.dst_width = dw;
				e.dst_height = dh;
			}
			expect(copier.composite(eyes), "composite a transparent overlay");
			read_pixels(dst, dw, dh, &px);
			expect(pixel_is(px, dw, dw / 2, dh / 2, 0, 0, 255), "transparent overlay: the scene shows (blue)");

			fill_ov(0.f, 1.f, 0.f, 1.f);
			expect(copier.composite(eyes), "composite an opaque overlay");
			read_pixels(dst, dw, dh, &px);
			expect(pixel_is(px, dw, 3, 3, 0, 255, 0), "opaque overlay covers the scene (green)");

			// 50% red over blue, blended in linear light: 0.5 linear is sRGB 188.
			fill_ov(1.f, 0.f, 0.f, 0.5f);
			expect(copier.composite(eyes), "composite a half-transparent overlay");
			read_pixels(dst, dw, dh, &px);
			const uint8_t *m = &px[((size_t)(dh / 2) * dw + dw / 2) * 4];
			printf("      half-transparent red over blue = %u,%u,%u\n", m[0], m[1], m[2]);
			expect(std::abs((int)m[0] - 188) <= 3 && m[1] == 0 && std::abs((int)m[2] - 188) <= 3,
			       "half-transparent overlay mixes in linear light (~188,0,188)");

			for (auto &e : eyes) {
				e.count = 1;
			}
			expect(copier.composite(eyes), "composite the scene alone");
			read_pixels(dst, dw, dh, &px);
			expect(pixel_is(px, dw, dw / 2, dh / 2, 0, 0, 255), "the scene alone (blue)");

			copier.forget(43);
			vk.DestroyImage(vk.device, ov, nullptr);
			vk.FreeMemory(vk.device, ov_mem, nullptr);
		}

		// FSR (fsr_render_scale): the 256x128 scene upscaled 2x into a 512x256 "swapchain".
		{
			const uint32_t uw = 2 * sw, uh = 2 * sh;
			VkImageCreateInfo up_ci = dst_ci;
			up_ci.extent = {uw, uh, 1};
			VkImage up;
			CHECK_VK(vk.CreateImage(vk.device, &up_ci, nullptr, &up));
			VkDeviceMemory up_mem = bind_memory(up, false);
			run([&] { barrier(up, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); });

			copier.set_upscale(true, 0.2f);
			VkCopier::CompositeEye eyes[2] = {};
			for (auto &e : eyes) {
				e.count = 1;
				e.src[0] = 42;
				e.rect[0][2] = 1.f;
				e.rect[0][3] = 1.f;
				e.dst = up;
				e.dst_width = uw;
				e.dst_height = uh;
			}
			fill(0.f, 0.f, 1.f, VK_IMAGE_LAYOUT_GENERAL); // blue
			expect(copier.composite(eyes), "FSR: composite + EASU + RCAS (256x128 -> 512x256)");
			expect(copier.upscaled(), "FSR: it upscaled (not a plain composite)");
			read_pixels(up, uw, uh, &px);
			bool flat = true;
			for (auto [x, y] : {std::pair<uint32_t, uint32_t>{0, 0}, {uw / 2, uh / 2}, {uw - 1, uh - 1}}) {
				flat = pixel_near(px, uw, x, y, 0, 0, 255, 1) && flat;
			}
			expect(flat, "FSR: a flat colour stays that colour (+-1), edges included (blue)");

			// Mid grey: FSR works on sRGB-encoded values and decodes after RCAS; 0.5 linear is sRGB 188.
			fill(0.5f, 0.5f, 0.5f, VK_IMAGE_LAYOUT_GENERAL);
			expect(copier.composite(eyes), "FSR: upscale mid grey");
			read_pixels(up, uw, uh, &px);
			const uint8_t *g = &px[((size_t)(uh / 2) * uw + uw / 2) * 4];
			printf("      FSR mid grey = %u,%u,%u\n", g[0], g[1], g[2]);
			expect(std::abs((int)g[0] - 188) <= 2 && std::abs((int)g[1] - 188) <= 2 && std::abs((int)g[2] - 188) <= 2,
			       "FSR: the sRGB round trip keeps the brightness (~188)");

			// Dynamic resolution: the game renders into a smaller part of its texture under load. The
			// region changes inside FSR's fixed-size input; its edge must not pick up what the last,
			// larger frame left beyond it (green there, blue now).
			fill(0.f, 1.f, 0.f, VK_IMAGE_LAYOUT_GENERAL); // green, full frame
			expect(copier.composite(eyes), "FSR: full frame (green)");
			fill(0.f, 0.f, 1.f, VK_IMAGE_LAYOUT_GENERAL); // blue
			for (auto &e : eyes) {
				e.rect[0][2] = 0.869f; // seen in HL2:VR
				e.rect[0][3] = 0.869f;
			}
			expect(copier.composite(eyes), "FSR: smaller region of the texture (dynamic resolution)");
			expect(copier.upscaled(), "FSR: still upscaling");
			read_pixels(up, uw, uh, &px);
			bool clean = true;
			for (auto [x, y] : {std::pair<uint32_t, uint32_t>{uw - 1, uh - 1}, {uw - 1, 0}, {0, uh - 1}, {uw / 2, uh / 2}}) {
				clean = pixel_near(px, uw, x, y, 0, 0, 255, 1) && clean;
			}
			expect(clean, "FSR: the smaller region fills the image, edges without the old frame (blue)");
			for (auto &e : eyes) {
				e.rect[0][2] = 1.f;
				e.rect[0][3] = 1.f;
			}

			// A scene as large as the swapchain still goes through FSR (1:1 + sharpening), so the look
			// doesn't flip between frames when dynamic resolution crosses the swapchain's size.
			for (auto &e : eyes) {
				e.dst = dst;
				e.dst_width = dw;
				e.dst_height = dh;
			}
			fill(0.f, 1.f, 0.f, VK_IMAGE_LAYOUT_GENERAL); // green
			expect(copier.composite(eyes), "FSR on, scene larger than the swapchain");
			expect(copier.upscaled(), "FSR: used every frame while on (1:1 pass)");
			read_pixels(dst, dw, dh, &px);
			expect(pixel_near(px, dw, dw / 2, dh / 2, 0, 255, 0, 1), "FSR: the downscaled scene arrives (green)");
			copier.set_upscale(false, 0.2f);

			vk.DestroyImage(vk.device, up, nullptr);
			vk.FreeMemory(vk.device, up_mem, nullptr);
		}

		printf("info: DMA-BUF write-fence waits used: %llu\n", (unsigned long long)copier.fence_waits());
		copier.forget(42);
		expect(!copier.has(42), "forget drops the import");
	}

	vk.DeviceWaitIdle(vk.device);
	vk.DestroyImage(vk.device, dst, nullptr);
	vk.FreeMemory(vk.device, dst_mem, nullptr);
	vk.DestroyImage(vk.device, src, nullptr);
	vk.FreeMemory(vk.device, src_mem, nullptr);
	vk.DestroyFence(vk.device, fence, nullptr);
	vk.DestroyCommandPool(vk.device, pool, nullptr);
	vk.destroy();
	printf("%s\n", failures == 0 ? "ALL OK" : "FAILURES");
	return failures == 0 ? 0 : 1;
}
