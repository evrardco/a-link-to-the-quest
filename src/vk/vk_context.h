// The Vulkan instance + device our OpenXR session renders with (created through
// XR_KHR_vulkan_enable2, so Monado picks the GPU and adds the extensions it needs).
//
// libvulkan.so.1 is dlopen'ed rather than linked: no Vulkan development package is needed to build,
// and the driver uses whichever loader SteamVR's runtime provides. Functions live in a table.
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace ml {

#define ML_VK_INSTANCE_FUNCS(X)                                                                                        \
	X(DestroyInstance)                                                                                             \
	X(GetDeviceProcAddr)                                                                                           \
	X(GetPhysicalDeviceProperties)                                                                                 \
	X(GetPhysicalDeviceQueueFamilyProperties)                                                                      \
	X(GetPhysicalDeviceMemoryProperties)                                                                           \
	X(EnumerateDeviceExtensionProperties)

#define ML_VK_DEVICE_FUNCS(X)                                                                                          \
	X(DestroyDevice)                                                                                               \
	X(GetDeviceQueue)                                                                                              \
	X(DeviceWaitIdle)                                                                                              \
	X(CreateCommandPool)                                                                                           \
	X(DestroyCommandPool)                                                                                          \
	X(AllocateCommandBuffers)                                                                                      \
	X(ResetCommandBuffer)                                                                                          \
	X(BeginCommandBuffer)                                                                                          \
	X(EndCommandBuffer)                                                                                            \
	X(CmdPipelineBarrier)                                                                                          \
	X(CmdBlitImage)                                                                                                \
	X(QueueSubmit)                                                                                                 \
	X(CreateFence)                                                                                                 \
	X(DestroyFence)                                                                                                \
	X(WaitForFences)                                                                                               \
	X(ResetFences)                                                                                                 \
	X(CreateImage)                                                                                                 \
	X(DestroyImage)                                                                                                \
	X(GetImageMemoryRequirements2)                                                                                 \
	X(AllocateMemory)                                                                                              \
	X(FreeMemory)                                                                                                  \
	X(BindImageMemory)                                                                                             \
	X(CreateSemaphore)                                                                                             \
	X(DestroySemaphore)                                                                                            \
	X(ImportSemaphoreFdKHR)                                                                                        \
	X(CreateBuffer)                                                                                                \
	X(DestroyBuffer)                                                                                               \
	X(GetBufferMemoryRequirements)                                                                                 \
	X(BindBufferMemory)                                                                                            \
	X(MapMemory)                                                                                                   \
	X(CmdCopyImageToBuffer)

struct VkContext
{
	void *lib = nullptr; //!< libvulkan.so.1
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
#define ML_VK_DECLARE(name) PFN_vk##name name = nullptr;
	ML_VK_INSTANCE_FUNCS(ML_VK_DECLARE)
	ML_VK_DEVICE_FUNCS(ML_VK_DECLARE)
#undef ML_VK_DECLARE

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	uint32_t queue_family = 0;
	VkQueue queue = VK_NULL_HANDLE;
	bool has_semaphore_fd = false; //!< VK_KHR_external_semaphore_fd: waiting on DMA-BUF write fences

	//! dlopen libvulkan and get vkGetInstanceProcAddr.
	bool
	load_library();
	//! After the instance exists (XR created it).
	void
	load_instance_functions();
	//! After the device exists; false if a required function is missing.
	bool
	load_device_functions();
	//! Destroys the device and instance (the runtime must be done with them) and unloads the library.
	void
	destroy();
	//! A memory type in `type_bits` with `flags`, or UINT32_MAX.
	uint32_t
	memory_type(uint32_t type_bits, VkMemoryPropertyFlags flags) const;
};

} // namespace ml
