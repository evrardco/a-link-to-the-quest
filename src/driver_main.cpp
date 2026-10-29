// Entry point: vrserver dlopens driver_monadolink.so and asks HmdDriverFactory for interfaces.
#include "device_provider.h"

#include <openvr_driver.h>

#include <cstring>

static ml::DeviceProvider g_device_provider;

extern "C" __attribute__((visibility("default"))) void *
HmdDriverFactory(const char *interface_name, int *return_code)
{
	if (strcmp(interface_name, vr::IServerTrackedDeviceProvider_Version) == 0) {
		return &g_device_provider;
	}

	if (return_code != nullptr) {
		*return_code = vr::VRInitError_Init_InterfaceNotFound;
	}
	return nullptr;
}
