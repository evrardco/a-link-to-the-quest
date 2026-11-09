// In-memory fakes of the SteamVR driver host interfaces, so driver code can run without SteamVR.
// FakeDriverContext::install() makes the vr::VRSettings()/VRProperties()/... accessors return these.
#pragma once

#include <openvr_driver.h>

#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fake {

class Settings : public vr::IVRSettings
{
public:
	std::map<std::pair<std::string, std::string>, std::string> strings;
	std::map<std::pair<std::string, std::string>, float> numbers; //!< bools, ints and floats
	std::map<std::pair<std::string, std::string>, float> default_numbers; //!< as load_defaults() left them

	const char *
	GetSettingsErrorNameFromEnum(vr::EVRSettingsError) override
	{
		return "fake";
	}
	void
	SetBool(const char *s, const char *k, bool v, vr::EVRSettingsError *e = nullptr) override
	{
		numbers[{s, k}] = v;
	}
	void
	SetInt32(const char *s, const char *k, int32_t v, vr::EVRSettingsError *e = nullptr) override
	{
		numbers[{s, k}] = (float)v;
	}
	void
	SetFloat(const char *s, const char *k, float v, vr::EVRSettingsError *e = nullptr) override
	{
		numbers[{s, k}] = v;
	}
	void
	SetString(const char *s, const char *k, const char *v, vr::EVRSettingsError *e = nullptr) override
	{
		strings[{s, k}] = v;
	}
	bool
	GetBool(const char *s, const char *k, vr::EVRSettingsError *e = nullptr) override
	{
		return number(s, k, e) != 0.f;
	}
	int32_t
	GetInt32(const char *s, const char *k, vr::EVRSettingsError *e = nullptr) override
	{
		return (int32_t)number(s, k, e);
	}
	float
	GetFloat(const char *s, const char *k, vr::EVRSettingsError *e = nullptr) override
	{
		return number(s, k, e);
	}
	void
	GetString(const char *s, const char *k, char *out, uint32_t len, vr::EVRSettingsError *e = nullptr) override
	{
		auto it = strings.find({s, k});
		if (e != nullptr) {
			*e = it == strings.end() ? vr::VRSettingsError_UnsetSettingHasNoDefault : vr::VRSettingsError_None;
		}
		snprintf(out, len, "%s", it == strings.end() ? "" : it->second.c_str());
	}
	void
	RemoveSection(const char *, vr::EVRSettingsError *e = nullptr) override
	{}
	void
	RemoveKeyInSection(const char *s, const char *k, vr::EVRSettingsError *e = nullptr) override
	{
		// Like SteamVR: the driver's default.vrsettings value applies again.
		numbers.erase({s, k});
		auto it = default_numbers.find({s, k});
		if (it != default_numbers.end()) {
			numbers[{s, k}] = it->second;
		}
	}

	//! The values from driver/resources/settings/default.vrsettings.
	void
	load_defaults()
	{
		SetString("driver_monadolink", "serial_number", "MONADOLINK-0001");
		SetString("driver_monadolink", "model_number", "Monado Link");
		SetInt32("driver_monadolink", "log_frames", 0);
		// Hermetic by default: never reach a real runtime (the tests that want one use the fake runtime).
		SetString("driver_monadolink", "openxr_runtime_json", "/nonexistent/monadolink-test-runtime.json");
		// Never the real ~/.cache/quest-vr/openxr_runtime.json either (a running Monado's manifest).
		SetString("driver_monadolink", "monado_runtime_file", "/nonexistent/monadolink-test-running-runtime.json");
		SetBool("driver_monadolink", "submit_frames", true);
		SetBool("driver_monadolink", "early_wait", false);
		SetBool("driver_monadolink", "overlay_layers", true);
		SetBool("driver_monadolink", "controllers", true);
		SetString("driver_monadolink", "controller_input_profile", "{oculus}/input/touch_profile.json");
		SetFloat("driver_monadolink", "controller_offset_z", -0.08f);
		SetFloat("driver_monadolink", "controller_offset_pitch", 22.f);
		SetBool("driver_monadolink", "controller_tuning", true);
		// Never the real ~/.cache/quest-vr/reset-stream: a running Monado would react to it.
		SetString("driver_monadolink", "stream_reset_file", "/nonexistent/monadolink-test-reset-stream");
		SetInt32("monadolink_display", "render_width", 1832);
		SetInt32("monadolink_display", "render_height", 1920);
		SetFloat("monadolink_display", "refresh_rate", 120.f);
		SetFloat("monadolink_display", "seconds_from_vsync_to_photons", 0.011f);
		SetFloat(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_IPD_Float, 0.063f);
		default_numbers = numbers;
	}

private:
	float
	number(const char *s, const char *k, vr::EVRSettingsError *e)
	{
		auto it = numbers.find({s, k});
		if (e != nullptr) {
			*e = it == numbers.end() ? vr::VRSettingsError_UnsetSettingHasNoDefault : vr::VRSettingsError_None;
		}
		return it == numbers.end() ? 0.f : it->second;
	}
};

class Properties : public vr::IVRProperties
{
public:
	struct Value
	{
		vr::PropertyTypeTag_t tag;
		std::vector<uint8_t> bytes;
	};
	//! container handle → property → value
	std::map<vr::PropertyContainerHandle_t, std::map<vr::ETrackedDeviceProperty, Value>> values;
	std::recursive_mutex mutex; //!< the tracking thread writes some properties (IPD changes)

	vr::ETrackedPropertyError
	ReadPropertyBatch(vr::PropertyContainerHandle_t c, vr::PropertyRead_t *batch, uint32_t count) override
	{
		std::lock_guard lock(mutex);
		for (uint32_t i = 0; i < count; i++) {
			auto &r = batch[i];
			auto it = values[c].find(r.prop);
			if (it == values[c].end()) {
				r.eError = vr::TrackedProp_UnknownProperty;
				continue;
			}
			r.unTag = it->second.tag;
			r.unRequiredBufferSize = (uint32_t)it->second.bytes.size();
			if (r.unBufferSize < it->second.bytes.size()) {
				r.eError = vr::TrackedProp_BufferTooSmall;
				continue;
			}
			memcpy(r.pvBuffer, it->second.bytes.data(), it->second.bytes.size());
			r.eError = vr::TrackedProp_Success;
		}
		return vr::TrackedProp_Success;
	}
	vr::ETrackedPropertyError
	WritePropertyBatch(vr::PropertyContainerHandle_t c, vr::PropertyWrite_t *batch, uint32_t count) override
	{
		std::lock_guard lock(mutex);
		for (uint32_t i = 0; i < count; i++) {
			auto &w = batch[i];
			if (w.writeType == vr::PropertyWrite_Set) {
				auto *p = (const uint8_t *)w.pvBuffer;
				values[c][w.prop] = Value{w.unTag, std::vector<uint8_t>(p, p + w.unBufferSize)};
			} else {
				values[c].erase(w.prop);
			}
			w.eError = vr::TrackedProp_Success;
		}
		return vr::TrackedProp_Success;
	}
	const char *
	GetPropErrorNameFromEnum(vr::ETrackedPropertyError) override
	{
		return "fake";
	}
	vr::PropertyContainerHandle_t
	TrackedDeviceToPropertyContainer(vr::TrackedDeviceIndex_t device) override
	{
		return 1000 + device;
	}
};

class ServerDriverHost : public vr::IVRServerDriverHost
{
public:
	struct Added
	{
		std::string serial;
		vr::ETrackedDeviceClass device_class;
		vr::ITrackedDeviceServerDriver *driver;
	};
	std::vector<Added> added;
	std::mutex mutex; //!< guards poses: the driver's tracking thread updates them
	std::vector<std::pair<uint32_t, vr::DriverPose_t>> poses;
	bool accept_devices = true;
	int eye_to_head_calls = 0;
	vr::HmdMatrix34_t eye_to_head[2] = {};

	std::vector<std::pair<uint32_t, vr::DriverPose_t>>
	poses_copy()
	{
		std::lock_guard lock(mutex);
		return poses;
	}

	bool
	TrackedDeviceAdded(const char *serial, vr::ETrackedDeviceClass c, vr::ITrackedDeviceServerDriver *d) override
	{
		if (!accept_devices) {
			return false;
		}
		added.push_back({serial, c, d});
		return true;
	}
	void
	TrackedDevicePoseUpdated(uint32_t which, const vr::DriverPose_t &pose, uint32_t size) override
	{
		std::lock_guard lock(mutex);
		poses.emplace_back(which, pose);
	}
	void
	VsyncEvent(double) override
	{}
	void
	VendorSpecificEvent(uint32_t, vr::EVREventType, const vr::VREvent_Data_t &, double) override
	{}
	bool
	IsExiting() override
	{
		return false;
	}
	std::vector<vr::VREvent_t> events; //!< handed out by PollNextEvent, oldest first
	bool
	PollNextEvent(vr::VREvent_t *out, uint32_t) override
	{
		std::lock_guard lock(mutex);
		if (events.empty()) {
			return false;
		}
		*out = events.front();
		events.erase(events.begin());
		return true;
	}
	void
	GetRawTrackedDevicePoses(float, vr::TrackedDevicePose_t *, uint32_t) override
	{}
	void
	RequestRestart(const char *, const char *, const char *, const char *) override
	{}
	uint32_t
	GetFrameTimings(vr::Compositor_FrameTiming *, uint32_t) override
	{
		return 0;
	}
	void
	SetDisplayEyeToHead(uint32_t, const vr::HmdMatrix34_t &left, const vr::HmdMatrix34_t &right) override
	{
		std::lock_guard lock(mutex); // also from the tracking thread (IPD changes)
		eye_to_head_calls++;
		eye_to_head[0] = left;
		eye_to_head[1] = right;
	}
	void
	SetDisplayProjectionRaw(uint32_t, const vr::HmdRect2_t &left, const vr::HmdRect2_t &right) override
	{
		std::lock_guard lock(mutex); // from the tracking thread (a late Monado connection)
		projection_left = left;
		projection_right = right;
	}
	void
	SetRecommendedRenderTargetSize(uint32_t, uint32_t width, uint32_t height) override
	{
		std::lock_guard lock(mutex);
		render_target_width = width;
		render_target_height = height;
	}
	vr::HmdRect2_t projection_left = {}, projection_right = {};
	uint32_t render_target_width = 0, render_target_height = 0;
};

class DriverLog : public vr::IVRDriverLog
{
public:
	//! SteamVR's logger is thread-safe and the driver logs from several threads (tracking, stderr
	//! capture): so is this one. Read with copy() while driver threads may run.
	std::mutex mutex;
	std::vector<std::string> lines;
	void
	Log(const char *message) override
	{
		std::lock_guard lock(mutex);
		lines.emplace_back(message);
	}
	std::vector<std::string>
	copy()
	{
		std::lock_guard lock(mutex);
		return lines;
	}
};

class Resources : public vr::IVRResources
{
public:
	uint32_t
	LoadSharedResource(const char *, char *, uint32_t) override
	{
		return 0;
	}
	uint32_t
	GetResourceFullPath(const char *, const char *, char *buf, uint32_t len) override
	{
		if (len > 0) {
			buf[0] = '\0';
		}
		return 0;
	}
};

class DriverManager : public vr::IVRDriverManager
{
public:
	uint32_t
	GetDriverCount() const override
	{
		return 1;
	}
	uint32_t
	GetDriverName(vr::DriverId_t, char *buf, uint32_t len) override
	{
		return (uint32_t)snprintf(buf, len, "monadolink") + 1;
	}
	vr::DriverHandle_t
	GetDriverHandle(const char *) override
	{
		return 1;
	}
	bool
	IsEnabled(vr::DriverId_t) const override
	{
		return true;
	}
};

class DriverInput : public vr::IVRDriverInput
{
public:
	struct Component
	{
		vr::PropertyContainerHandle_t container;
		std::string path;
		enum
		{
			Bool,
			Scalar,
			Haptic
		} kind;
		vr::EVRScalarUnits units;
		float value;
		int updates;
	};
	std::mutex mutex; //!< the tracking thread updates components
	std::map<vr::VRInputComponentHandle_t, Component> components;
	vr::VRInputComponentHandle_t next = 1;

	//! The component at `path` on `container`, or null.
	const Component *
	find(vr::PropertyContainerHandle_t container, const std::string &path, vr::VRInputComponentHandle_t *handle = nullptr)
	{
		std::lock_guard lock(mutex);
		for (auto &[h, c] : components) {
			if (c.container == container && c.path == path) {
				if (handle != nullptr) {
					*handle = h;
				}
				return &c;
			}
		}
		return nullptr;
	}
	//! Thread-safe copy of a component's value (-1 if absent).
	float
	value(vr::PropertyContainerHandle_t container, const std::string &path)
	{
		std::lock_guard lock(mutex);
		for (auto &[h, c] : components) {
			if (c.container == container && c.path == path) {
				return c.value;
			}
		}
		return -1.f;
	}

	vr::EVRInputError
	CreateBooleanComponent(vr::PropertyContainerHandle_t c, const char *name, vr::VRInputComponentHandle_t *h) override
	{
		return add(c, name, Component::Bool, vr::VRScalarUnits_NormalizedOneSided, h);
	}
	vr::EVRInputError
	UpdateBooleanComponent(vr::VRInputComponentHandle_t h, bool v, double) override
	{
		return update(h, v ? 1.f : 0.f);
	}
	vr::EVRInputError
	CreateScalarComponent(vr::PropertyContainerHandle_t c, const char *name, vr::VRInputComponentHandle_t *h,
	                      vr::EVRScalarType, vr::EVRScalarUnits units) override
	{
		return add(c, name, Component::Scalar, units, h);
	}
	vr::EVRInputError
	UpdateScalarComponent(vr::VRInputComponentHandle_t h, float v, double) override
	{
		return update(h, v);
	}
	vr::EVRInputError
	CreateHapticComponent(vr::PropertyContainerHandle_t c, const char *name, vr::VRInputComponentHandle_t *h) override
	{
		return add(c, name, Component::Haptic, vr::VRScalarUnits_NormalizedOneSided, h);
	}
	vr::EVRInputError
	CreateSkeletonComponent(vr::PropertyContainerHandle_t, const char *, const char *, const char *,
	                        vr::EVRSkeletalTrackingLevel, const vr::VRBoneTransform_t *, uint32_t,
	                        vr::VRInputComponentHandle_t *) override
	{
		return vr::VRInputError_InvalidParam;
	}
	vr::EVRInputError
	UpdateSkeletonComponent(vr::VRInputComponentHandle_t, vr::EVRSkeletalMotionRange, const vr::VRBoneTransform_t *,
	                        uint32_t) override
	{
		return vr::VRInputError_InvalidParam;
	}
	vr::EVRInputError
	CreatePoseComponent(vr::PropertyContainerHandle_t, const char *, vr::VRInputComponentHandle_t *) override
	{
		return vr::VRInputError_InvalidParam;
	}
	vr::EVRInputError
	UpdatePoseComponent(vr::VRInputComponentHandle_t, const vr::HmdMatrix34_t *, double) override
	{
		return vr::VRInputError_InvalidParam;
	}
	vr::EVRInputError
	CreateEyeTrackingComponent(vr::PropertyContainerHandle_t, const char *, vr::VRInputComponentHandle_t *) override
	{
		return vr::VRInputError_InvalidParam;
	}
	vr::EVRInputError
	UpdateEyeTrackingComponent(vr::VRInputComponentHandle_t, const vr::VREyeTrackingData_t *, double) override
	{
		return vr::VRInputError_InvalidParam;
	}

private:
	vr::EVRInputError
	add(vr::PropertyContainerHandle_t c, const char *name, decltype(Component::kind) kind, vr::EVRScalarUnits units,
	    vr::VRInputComponentHandle_t *h)
	{
		std::lock_guard lock(mutex);
		*h = next++;
		components[*h] = Component{c, name, kind, units, 0.f, 0};
		return vr::VRInputError_None;
	}
	vr::EVRInputError
	update(vr::VRInputComponentHandle_t h, float v)
	{
		std::lock_guard lock(mutex);
		auto it = components.find(h);
		if (it == components.end()) {
			return vr::VRInputError_InvalidHandle;
		}
		it->second.value = v;
		it->second.updates++;
		return vr::VRInputError_None;
	}
};

class IPCResourceManager : public vr::IVRIPCResourceManagerClient
{
public:
	struct Image
	{
		uint32_t format, width, height;
		bool renderable;
		uint32_t create_flags, usage_flags;
		int refs; //!< released when it drops to 0
	};
	std::map<vr::SharedTextureHandle_t, Image> images; //!< live images
	std::vector<vr::SharedTextureHandle_t> unrefs;
	int fail_after = -1; //!< fail the Nth NewSharedVulkanImage (0-based), -1 = never
	int created = 0;
	vr::SharedTextureHandle_t next_handle = 100;
	bool share_fds = false; //!< RefResource/ReceiveSharedFd succeed, with /dev/null fds
	int fds_given = 0;

	bool
	NewSharedVulkanImage(uint32_t format, uint32_t w, uint32_t h, bool renderable, bool mappable, bool compute,
	                     uint32_t mips, uint32_t layers, uint32_t create_flags, uint32_t usage_flags,
	                     vr::SharedTextureHandle_t *out) override
	{
		if (fail_after >= 0 && created >= fail_after) {
			return false;
		}
		created++;
		*out = next_handle++;
		images[*out] = Image{format, w, h, renderable, create_flags, usage_flags, 1};
		return true;
	}
	bool
	NewSharedVulkanBuffer(uint32_t, uint32_t, vr::SharedTextureHandle_t *) override
	{
		return false;
	}
	bool
	NewSharedVulkanSemaphore(bool, vr::SharedTextureHandle_t *) override
	{
		return false;
	}
	bool
	RefResource(vr::SharedTextureHandle_t handle, uint64_t *ipc_handle) override
	{
		auto it = images.find(handle);
		if (!share_fds || it == images.end()) {
			return false;
		}
		it->second.refs++;
		if (ipc_handle != nullptr) {
			*ipc_handle = 0x1000 + handle;
		}
		return true;
	}
	bool
	UnrefResource(vr::SharedTextureHandle_t handle) override
	{
		unrefs.push_back(handle);
		auto it = images.find(handle);
		if (it == images.end()) {
			return false;
		}
		if (--it->second.refs == 0) {
			images.erase(it);
		}
		return true;
	}
	bool
	GetDmabufFormats(uint32_t *count, uint32_t *) override
	{
		*count = 0;
		return false;
	}
	bool
	GetDmabufModifiers(vr::EVRApplicationType, uint32_t, uint32_t *count, uint64_t *) override
	{
		*count = 0;
		return false;
	}
	bool
	ImportDmabuf(vr::EVRApplicationType, vr::DmabufAttributes_t *, vr::SharedTextureHandle_t *) override
	{
		return false;
	}
	bool
	ReceiveSharedFd(uint64_t, int *fd) override
	{
		if (!share_fds) {
			return false;
		}
		*fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		fds_given++;
		return *fd >= 0;
	}
};

class DriverContext : public vr::IVRDriverContext
{
public:
	Settings settings;
	Properties properties;
	ServerDriverHost host;
	DriverLog log;
	Resources resources;
	DriverManager manager;
	IPCResourceManager ipc;
	DriverInput input;

	void *
	GetGenericInterface(const char *version, vr::EVRInitError *error = nullptr) override
	{
		if (error != nullptr) {
			*error = vr::VRInitError_None;
		}
		if (strcmp(version, vr::IVRSettings_Version) == 0) {
			return static_cast<vr::IVRSettings *>(&settings);
		}
		if (strcmp(version, vr::IVRProperties_Version) == 0) {
			return static_cast<vr::IVRProperties *>(&properties);
		}
		if (strcmp(version, vr::IVRServerDriverHost_Version) == 0) {
			return static_cast<vr::IVRServerDriverHost *>(&host);
		}
		if (strcmp(version, vr::IVRDriverLog_Version) == 0) {
			return static_cast<vr::IVRDriverLog *>(&log);
		}
		if (strcmp(version, vr::IVRResources_Version) == 0) {
			return static_cast<vr::IVRResources *>(&resources);
		}
		if (strcmp(version, vr::IVRDriverManager_Version) == 0) {
			return static_cast<vr::IVRDriverManager *>(&manager);
		}
		if (strcmp(version, vr::IVRDriverInput_Version) == 0) {
			return static_cast<vr::IVRDriverInput *>(&input);
		}
		if (strcmp(version, vr::IVRIPCResourceManagerClient_Version) == 0) {
			return static_cast<vr::IVRIPCResourceManagerClient *>(&ipc);
		}
		if (error != nullptr) {
			*error = vr::VRInitError_Init_InterfaceNotFound;
		}
		return nullptr;
	}
	vr::DriverHandle_t
	GetDriverHandle() override
	{
		return 1;
	}

	//! Load default settings and point the vr:: accessors at this context.
	void
	install()
	{
		settings.load_defaults();
		vr::InitServerDriverContext(this);
	}
	~DriverContext()
	{
		vr::CleanupDriverContext();
	}
};

//! Read a typed property written by the driver through vr::VRProperties().
template <typename T>
T
get_property(Properties &props, vr::PropertyContainerHandle_t c, vr::ETrackedDeviceProperty prop)
{
	T value{};
	auto it = props.values[c].find(prop);
	if (it != props.values[c].end() && it->second.bytes.size() == sizeof(T)) {
		memcpy(&value, it->second.bytes.data(), sizeof(T));
	}
	return value;
}

inline std::string
get_string_property(Properties &props, vr::PropertyContainerHandle_t c, vr::ETrackedDeviceProperty prop)
{
	auto it = props.values[c].find(prop);
	if (it == props.values[c].end()) {
		return {};
	}
	return std::string((const char *)it->second.bytes.data()); // stored with its terminating NUL
}

} // namespace fake
