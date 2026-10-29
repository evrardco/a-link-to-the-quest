// HmdDriverFactory: the only symbol vrserver looks up in driver_monadolink.so.
#include <doctest/doctest.h>
#include <openvr_driver.h>

extern "C" void *
HmdDriverFactory(const char *interface_name, int *return_code);

TEST_CASE("factory returns the device provider for the provider interface version")
{
	int error = -1;
	void *provider = HmdDriverFactory(vr::IServerTrackedDeviceProvider_Version, &error);
	CHECK(provider != nullptr);
	CHECK(HmdDriverFactory(vr::IServerTrackedDeviceProvider_Version, nullptr) == provider); // same instance
}

TEST_CASE("factory rejects unknown interfaces with InterfaceNotFound")
{
	int error = 0;
	CHECK(HmdDriverFactory("IVRWatchdogProvider_001", &error) == nullptr);
	CHECK(error == vr::VRInitError_Init_InterfaceNotFound);
	CHECK(HmdDriverFactory("IServerTrackedDeviceProvider_003", nullptr) == nullptr); // old version, null error ptr
}
