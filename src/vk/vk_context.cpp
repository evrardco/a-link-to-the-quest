#include "vk/vk_context.h"

#include "util/log.h"

#include <dlfcn.h>
#include <string_view>

namespace ml {

bool
VkContext::load_library()
{
	if (lib == nullptr) {
		lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	}
	if (lib == nullptr) {
		log("Vulkan: can't load libvulkan.so.1: %s", dlerror());
		return false;
	}
	GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
	return GetInstanceProcAddr != nullptr;
}

void
VkContext::load_instance_functions()
{
#define ML_VK_LOAD(name) name = (PFN_vk##name)GetInstanceProcAddr(instance, "vk" #name);
	ML_VK_INSTANCE_FUNCS(ML_VK_LOAD)
#undef ML_VK_LOAD
}

bool
VkContext::load_device_functions()
{
	bool ok = true;
#define ML_VK_LOAD(name)                                                                                               \
	name = (PFN_vk##name)GetDeviceProcAddr(device, "vk" #name);                                                    \
	if (name == nullptr && std::string_view(#name) != "ImportSemaphoreFdKHR") {                                     \
		log("Vulkan: missing vk" #name);                                                                       \
		ok = false;                                                                                            \
	}
	ML_VK_DEVICE_FUNCS(ML_VK_LOAD)
#undef ML_VK_LOAD
	has_semaphore_fd = has_semaphore_fd && ImportSemaphoreFdKHR != nullptr;
	if (ok) {
		GetDeviceQueue(device, queue_family, 0, &queue);
	}
	return ok;
}

void
VkContext::destroy()
{
	if (device != VK_NULL_HANDLE && DestroyDevice != nullptr) {
		DestroyDevice(device, nullptr);
	}
	device = VK_NULL_HANDLE;
	queue = VK_NULL_HANDLE;
	if (instance != VK_NULL_HANDLE && DestroyInstance != nullptr) {
		DestroyInstance(instance, nullptr);
	}
	instance = VK_NULL_HANDLE;
	physical_device = VK_NULL_HANDLE;
	// The library stays loaded: Monado's client may still hold function pointers into it.
}

uint32_t
VkContext::memory_type(uint32_t type_bits, VkMemoryPropertyFlags flags) const
{
	VkPhysicalDeviceMemoryProperties props;
	GetPhysicalDeviceMemoryProperties(physical_device, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) != 0 && (props.memoryTypes[i].propertyFlags & flags) == flags) {
			return i;
		}
	}
	return UINT32_MAX;
}

} // namespace ml
