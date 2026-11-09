// SPDX-License-Identifier: MIT
#include "xr/xr_client.h"

#include "util/log.h"
#include "xr/xr_math.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace ml {

//! Standing eye height used when the runtime has no STAGE space and we fall back to LOCAL (head origin).
static constexpr double k_local_to_standing_y = 1.6;

static bool
is_loss(XrResult result)
{
	return result == XR_ERROR_INSTANCE_LOST || result == XR_ERROR_SESSION_LOST;
}

std::string
runtime_manifest_path()
{
	// The loader's search order on Linux: XR_RUNTIME_JSON, then active_runtime.json in
	// $XDG_CONFIG_HOME (~/.config), $XDG_CONFIG_DIRS (/etc/xdg) and /etc.
	if (const char *env = getenv("XR_RUNTIME_JSON"); env != nullptr && env[0] != '\0') {
		return env;
	}
	std::vector<std::string> dirs;
	if (const char *c = getenv("XDG_CONFIG_HOME"); c != nullptr && c[0] != '\0') {
		dirs.push_back(c);
	} else if (const char *home = getenv("HOME"); home != nullptr) {
		dirs.push_back(std::string(home) + "/.config");
	}
	std::string config_dirs = getenv("XDG_CONFIG_DIRS") != nullptr ? getenv("XDG_CONFIG_DIRS") : "/etc/xdg";
	for (size_t start = 0; start <= config_dirs.size();) {
		size_t end = config_dirs.find(':', start);
		if (end == std::string::npos) {
			end = config_dirs.size();
		}
		if (end > start) {
			dirs.push_back(config_dirs.substr(start, end - start));
		}
		start = end + 1;
	}
	dirs.push_back("/etc");
	for (const auto &dir : dirs) {
		std::string path = dir + "/openxr/1/active_runtime.json";
		if (access(path.c_str(), R_OK) == 0) {
			return path;
		}
	}
	return {};
}

bool
is_steamvr_manifest(const std::string &path)
{
	FILE *f = path.empty() ? nullptr : fopen(path.c_str(), "r");
	if (f == nullptr) {
		return false;
	}
	std::string text;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		text.append(buf, n);
	}
	fclose(f);
	// SteamVR's manifest is steamxr_linux64.json and points at vrclient.so.
	return path.find("steamxr") != std::string::npos || text.find("vrclient") != std::string::npos ||
	       text.find("SteamVR") != std::string::npos;
}

XrClient::~XrClient()
{
	stop();
}

bool
XrClient::check(XrResult result, const char *what)
{
	if (XR_SUCCEEDED(result)) {
		return true;
	}
	char name[XR_MAX_RESULT_STRING_SIZE] = "";
	if (instance_ == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance_, result, name))) {
		snprintf(name, sizeof(name), "XrResult %d", (int)result);
	}
	log("OpenXR: %s failed: %s", what, name);
	if (is_loss(result)) {
		lost_ = true;
	}
	return false;
}

bool
XrClient::start(const char *runtime_json, bool want_frames)
{
	stop();
	lost_ = false;
	runtime_json_ = runtime_json != nullptr ? runtime_json : "";
	want_frames_ = want_frames;

	if (runtime_json != nullptr && runtime_json[0] != '\0') {
		setenv("XR_RUNTIME_JSON", runtime_json, 1);
	}

	// Inside vrserver the default OpenXR runtime is usually SteamVR itself: its client would connect
	// back to vrserver, which waits for our Init to return, and time out after 20 s. Refuse it.
	std::string manifest = runtime_manifest_path();
	log("OpenXR: runtime manifest %s", manifest.empty() ? "(none found)" : manifest.c_str());
	if (is_steamvr_manifest(manifest)) {
		log("OpenXR: that is SteamVR's own runtime, not Monado: set driver_monadolink/openxr_runtime_json "
		    "to Monado's manifest (scripts/install.sh does it)");
		return false;
	}

	// Required: CLOCK_MONOTONIC <-> XrTime conversion, and a Vulkan session (to submit SteamVR's frames)
	// or at least a headless one (tracking only).
	uint32_t count = 0;
	if (!check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "listing extensions")) {
		return false;
	}
	std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());
	auto has = [&](const char *name) {
		for (const auto &p : props) {
			if (strcmp(p.extensionName, name) == 0) {
				return true;
			}
		}
		return false;
	};
	std::vector<const char *> exts;
	if (!has(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME)) {
		log("OpenXR: the runtime lacks " XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
		return false;
	}
	exts.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
	const bool has_vulkan = want_frames && has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	const bool has_headless = has(XR_MND_HEADLESS_EXTENSION_NAME);
	if (!has_vulkan && !has_headless) {
		log("OpenXR: the runtime has neither " XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME " nor " XR_MND_HEADLESS_EXTENSION_NAME);
		return false;
	}
	if (has_vulkan) {
		exts.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	}
	if (has_headless) {
		exts.push_back(XR_MND_HEADLESS_EXTENSION_NAME);
	}
	const bool has_refresh_rate = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if (has_refresh_rate) {
		exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	}

	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "monadolink");
	snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "SteamVR");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0; // the widest runtime support
	ici.enabledExtensionCount = (uint32_t)exts.size();
	ici.enabledExtensionNames = exts.data();
	if (!check(xrCreateInstance(&ici, &instance_), "xrCreateInstance")) {
		instance_ = XR_NULL_HANDLE;
		return false;
	}

	XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
	if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &ip))) {
		log("OpenXR: runtime \"%s\" %u.%u.%u (%s)", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
		    XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion), process_tag());
	}

	xrGetInstanceProcAddr(instance_, "xrConvertTimespecTimeToTimeKHR", (PFN_xrVoidFunction *)&convert_timespec_);
	if (has_refresh_rate) {
		xrGetInstanceProcAddr(instance_, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction *)&get_refresh_rate_);
	}

	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!check(xrGetSystem(instance_, &sgi, &system_), "xrGetSystem (is the headset connected?)")) {
		stop();
		return false;
	}

	count = 2;
	for (auto &v : views_) {
		v = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
	}
	if (!check(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
	                                             &count, views_),
	           "xrEnumerateViewConfigurationViews")) {
		stop();
		return false;
	}

	// A Vulkan session when we can (frames), else headless: no graphics binding in the chain, which
	// with XR_MND_headless enabled makes a headless session (tracking only).
	XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = system_;
	if (has_vulkan && create_vulkan()) {
		binding.instance = vk_.instance;
		binding.physicalDevice = vk_.physical_device;
		binding.device = vk_.device;
		binding.queueFamilyIndex = vk_.queue_family;
		binding.queueIndex = 0;
		sci.next = &binding;
		if (check(xrCreateSession(instance_, &sci, &session_), "xrCreateSession (Vulkan)")) {
			vulkan_ = true;
		} else {
			session_ = XR_NULL_HANDLE;
			vk_.destroy();
			sci.next = nullptr;
		}
	}
	if (!vulkan_) {
		if (!has_headless) {
			stop();
			return false;
		}
		if (!check(xrCreateSession(instance_, &sci, &session_), "xrCreateSession (headless)")) {
			session_ = XR_NULL_HANDLE;
			stop();
			return false;
		}
	}

	count = 0;
	xrEnumerateReferenceSpaces(session_, 0, &count, nullptr);
	std::vector<XrReferenceSpaceType> types(count);
	xrEnumerateReferenceSpaces(session_, count, &count, types.data());
	XrReferenceSpaceType base_type = XR_REFERENCE_SPACE_TYPE_LOCAL;
	base_offset_y_ = k_local_to_standing_y;
	for (auto t : types) {
		if (t == XR_REFERENCE_SPACE_TYPE_STAGE) {
			base_type = XR_REFERENCE_SPACE_TYPE_STAGE;
			base_offset_y_ = 0.0;
		}
	}

	XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rci.poseInReferenceSpace.orientation.w = 1.f;
	rci.referenceSpaceType = base_type;
	if (!check(xrCreateReferenceSpace(session_, &rci, &base_space_), "creating the base space")) {
		base_space_ = XR_NULL_HANDLE;
		stop();
		return false;
	}
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!check(xrCreateReferenceSpace(session_, &rci, &view_space_), "creating the view space")) {
		view_space_ = XR_NULL_HANDLE;
		stop();
		return false;
	}
	log("OpenXR: %s session up, base space %s", vulkan_ ? "Vulkan" : "headless",
	    base_type == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL");
	if (vulkan_ && !create_swapchains()) {
		log("OpenXR: no swapchains: tracking only, SteamVR's frames won't be shown");
	}
	if (!input_.init(instance_, session_)) {
		log("OpenXR: no controller input (the HMD still works)");
	}

	poll_events(); // the runtime queues READY right away: begin now
	return true;
}

void
XrClient::apply_display_info(DisplayConfig *c)
{
	if (session_ == XR_NULL_HANDLE) {
		return;
	}

	const uint32_t w = views_[0].recommendedImageRectWidth > views_[1].recommendedImageRectWidth
	                       ? views_[0].recommendedImageRectWidth
	                       : views_[1].recommendedImageRectWidth;
	const uint32_t h = views_[0].recommendedImageRectHeight > views_[1].recommendedImageRectHeight
	                       ? views_[0].recommendedImageRectHeight
	                       : views_[1].recommendedImageRectHeight;
	if (w > 0 && h > 0) {
		c->render_width = w;
		c->render_height = h;
	}

	float rate = 0.f;
	if (get_refresh_rate_ != nullptr && XR_SUCCEEDED(get_refresh_rate_(session_, &rate)) && rate > 0.f) {
		c->refresh_rate = rate;
	}

	XrPosef poses[2];
	XrFovf fovs[2];
	if (locate_eyes(poses, fovs)) {
		for (int eye = 0; eye < 2; eye++) {
			fov_[eye] = fovs[eye];
			projection_from_xr_fov(fovs[eye], &c->tan_left[eye], &c->tan_right[eye], &c->tan_top[eye],
			                       &c->tan_bottom[eye]);
		}
		float ipd = 0.f;
		if (!plausible_ipd(poses, &ipd)) {
			// Keep the default eye poses (64 mm) until the tracking thread sees a real IPD.
			log("Runtime reports an implausible IPD (%.1f mm): using %.0f mm until it reports a real one",
			    ipd * 1000.f, (eye_pose_[1].position.x - eye_pose_[0].position.x) * 1000.f);
			poses[0] = eye_pose_[0];
			poses[1] = eye_pose_[1];
			plausible_ipd(poses, &ipd);
		}
		{
			std::lock_guard lock(eye_mutex_);
			eye_pose_[0] = poses[0];
			eye_pose_[1] = poses[1];
		}
		c->ipd_meters = ipd;
		c->eye_to_head[0] = matrix_from_xr_pose(poses[0]);
		c->eye_to_head[1] = matrix_from_xr_pose(poses[1]);
		c->has_eye_to_head = true;
	}

	log("OpenXR display: %ux%u per eye @ %.1f Hz, IPD %.1f mm, left eye tan l/r/t/b %.3f/%.3f/%.3f/%.3f",
	    c->render_width, c->render_height, c->refresh_rate, c->ipd_meters * 1000.f, c->tan_left[0], c->tan_right[0],
	    c->tan_top[0], c->tan_bottom[0]);
}

bool
XrClient::locate_eyes(XrPosef poses[2], XrFovf fovs[2])
{
	XrTime t = 0;
	if (!now(&t)) {
		return false;
	}
	XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
	vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	vli.space = view_space_; // eye poses relative to the head
	vli.displayTime = t;
	XrViewState state{XR_TYPE_VIEW_STATE};
	XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	uint32_t count = 0;
	if (!check(xrLocateViews(session_, &vli, &state, 2, &count, views), "xrLocateViews") || count != 2) {
		return false;
	}
	const XrViewStateFlags pose_bits = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
	if ((state.viewStateFlags & pose_bits) != pose_bits) {
		return false;
	}
	for (int eye = 0; eye < 2; eye++) {
		poses[eye] = views[eye].pose;
		fovs[eye] = views[eye].fov;
	}
	return true;
}

bool
XrClient::refresh_eye_poses(XrPosef out[2])
{
	XrFovf fovs[2];
	float ipd = 0.f;
	if (instance_ == XR_NULL_HANDLE || lost_ || !locate_eyes(out, fovs) || !plausible_ipd(out, &ipd)) {
		return false;
	}
	std::lock_guard lock(eye_mutex_);
	bool changed = false;
	for (int eye = 0; eye < 2; eye++) {
		const XrVector3f &a = out[eye].position, &b = eye_pose_[eye].position;
		changed |= std::fabs(a.x - b.x) > 0.0002f || std::fabs(a.y - b.y) > 0.0002f || std::fabs(a.z - b.z) > 0.0002f;
	}
	if (changed) {
		eye_pose_[0] = out[0];
		eye_pose_[1] = out[1];
	}
	return changed;
}

void
XrClient::poll_events()
{
	if (instance_ == XR_NULL_HANDLE || lost_) {
		return;
	}
	XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
	XrResult result;
	while ((result = xrPollEvent(instance_, &ev)) == XR_SUCCESS) {
		if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			log("OpenXR: instance loss pending (Monado stopping?)");
			running_ = false;
			lost_ = true;
		} else if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const auto &e = *(const XrEventDataSessionStateChanged *)&ev;
			switch (e.state) {
			case XR_SESSION_STATE_READY: {
				std::lock_guard lock(frame_mutex_); // never between a frame's wait and end
				XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				running_ = check(xrBeginSession(session_, &bi), "xrBeginSession");
				log("OpenXR: session begun");
				break;
			}
			case XR_SESSION_STATE_STOPPING: {
				std::lock_guard lock(frame_mutex_);
				running_ = false;
				check(xrEndSession(session_), "xrEndSession");
				log("OpenXR: session ended");
				break;
			}
			case XR_SESSION_STATE_LOSS_PENDING:
			case XR_SESSION_STATE_EXITING:
				log("OpenXR: session state %d: lost", (int)e.state);
				running_ = false;
				lost_ = true;
				break;
			default: break;
			}
		}
		ev = {XR_TYPE_EVENT_DATA_BUFFER};
	}
	if (XR_FAILED(result)) {
		check(result, "xrPollEvent");
	}
}

bool
XrClient::now(XrTime *out)
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return convert_timespec_ != nullptr && check(convert_timespec_(instance_, &ts, out), "converting the time");
}

bool
XrClient::locate_head(vr::DriverPose_t *out)
{
	if (session_ == XR_NULL_HANDLE || lost_) {
		return false;
	}
	XrTime t = 0;
	if (!now(&t)) {
		return false;
	}
	// "Now" rather than a predicted display time: SteamVR extrapolates from the velocities itself.
	XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION, &vel};
	XrResult result = xrLocateSpace(view_space_, base_space_, t, &loc);
	if (XR_FAILED(result)) {
		// Called hundreds of times a second: log the first failure and then every 1000th.
		if (locate_failures_++ % 1000 == 0) {
			check(result, "xrLocateSpace (head)");
		} else if (is_loss(result)) {
			lost_ = true;
		}
		return false;
	}
	*out = driver_pose_from_xr(loc, vel, world_offset_y());
	if ((loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
		std::lock_guard lock(head_mutex_);
		last_head_ = loc.pose;
		have_head_ = true;
	}
	return true;
}

bool
XrClient::last_head(XrPosef *out)
{
	std::lock_guard lock(head_mutex_);
	*out = last_head_;
	return have_head_;
}

bool
XrClient::sync_hands(const XrPosef offset[2], HandState out[2])
{
	XrTime t = 0;
	if (session_ == XR_NULL_HANDLE || lost_ || !input_.ready() || !now(&t)) {
		for (int hand = 0; hand < 2; hand++) {
			out[hand] = {};
		}
		return false;
	}
	return input_.sync(t, base_space_, world_offset_y(), offset, out);
}

void
XrClient::stop()
{
	input_.destroy();
	for (int layer = 0; layer < k_max_layers; layer++) {
		for (int eye = 0; eye < 2; eye++) {
			if (swapchains_[layer][eye] != XR_NULL_HANDLE) {
				xrDestroySwapchain(swapchains_[layer][eye]);
				swapchains_[layer][eye] = XR_NULL_HANDLE;
			}
			swapchain_images_[layer][eye].clear();
		}
	}
	if (view_space_ != XR_NULL_HANDLE) {
		xrDestroySpace(view_space_);
		view_space_ = XR_NULL_HANDLE;
	}
	if (base_space_ != XR_NULL_HANDLE) {
		xrDestroySpace(base_space_);
		base_space_ = XR_NULL_HANDLE;
	}
	if (session_ != XR_NULL_HANDLE) {
		xrDestroySession(session_);
		session_ = XR_NULL_HANDLE;
	}
	if (instance_ != XR_NULL_HANDLE) {
		xrDestroyInstance(instance_);
		instance_ = XR_NULL_HANDLE;
	}
	// Our Vulkan device outlives the session and instance that used it.
	vk_.destroy();
	vulkan_ = false;
	system_ = XR_NULL_SYSTEM_ID;
	running_ = false;
	convert_timespec_ = nullptr;
	get_refresh_rate_ = nullptr;
	locate_failures_ = 0;
	std::lock_guard lock(head_mutex_);
	have_head_ = false; // the pose guard must not compare against the old session's head
}

bool
XrClient::restart()
{
	std::unique_lock restart_lock(restart_mutex_);
	{
		// The submit thread is between frames while we hold this; it sees restarting_ until we're done.
		std::lock_guard lock(frame_mutex_);
		restarting_ = true;
		if (teardown_hook_) {
			teardown_hook_();
		}
		stop();
	}
	// Not under frame_mutex_: start() takes it to begin the session.
	const std::string runtime_json = runtime_json_;
	const bool ok = start(runtime_json.c_str(), want_frames_);
	if (!ok) {
		lost_ = true; // still gone: the tracking thread keeps trying
	}
	restarting_ = false;
	return ok;
}

bool
XrClient::create_vulkan()
{
	PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = nullptr;
	PFN_xrCreateVulkanInstanceKHR create_instance = nullptr;
	PFN_xrGetVulkanGraphicsDevice2KHR get_device = nullptr;
	PFN_xrCreateVulkanDeviceKHR create_device = nullptr;
	xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&get_requirements);
	xrGetInstanceProcAddr(instance_, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&create_instance);
	xrGetInstanceProcAddr(instance_, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&get_device);
	xrGetInstanceProcAddr(instance_, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&create_device);
	if (!get_requirements || !create_instance || !get_device || !create_device || !vk_.load_library()) {
		return false;
	}

	XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	if (!check(get_requirements(instance_, system_, &req), "xrGetVulkanGraphicsRequirements2KHR")) {
		return false;
	}

	// Vulkan 1.1: external memory/semaphore capabilities and dedicated allocations are core.
	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "monadolink";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	vici.pApplicationInfo = &app;
	XrVulkanInstanceCreateInfoKHR xvici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	xvici.systemId = system_;
	xvici.pfnGetInstanceProcAddr = vk_.GetInstanceProcAddr;
	xvici.vulkanCreateInfo = &vici;
	VkResult vr_result = VK_SUCCESS;
	if (!check(create_instance(instance_, &xvici, &vk_.instance, &vr_result), "xrCreateVulkanInstanceKHR") ||
	    vr_result != VK_SUCCESS) {
		log("Vulkan: instance creation failed (VkResult %d)", (int)vr_result);
		vk_.instance = VK_NULL_HANDLE;
		return false;
	}
	vk_.load_instance_functions();

	// Monado picks the GPU (the one it renders and encodes on).
	XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	gdi.systemId = system_;
	gdi.vulkanInstance = vk_.instance;
	if (!check(get_device(instance_, &gdi, &vk_.physical_device), "xrGetVulkanGraphicsDevice2KHR")) {
		vk_.destroy();
		return false;
	}
	VkPhysicalDeviceProperties props;
	vk_.GetPhysicalDeviceProperties(vk_.physical_device, &props);

	// A graphics queue: vkCmdBlitImage needs one.
	uint32_t family_count = 0;
	vk_.GetPhysicalDeviceQueueFamilyProperties(vk_.physical_device, &family_count, nullptr);
	std::vector<VkQueueFamilyProperties> families(family_count);
	vk_.GetPhysicalDeviceQueueFamilyProperties(vk_.physical_device, &family_count, families.data());
	vk_.queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < family_count; i++) {
		if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
			vk_.queue_family = i;
			break;
		}
	}
	if (vk_.queue_family == UINT32_MAX) {
		log("Vulkan: no graphics queue on %s", props.deviceName);
		vk_.destroy();
		return false;
	}

	uint32_t ext_count = 0;
	vk_.EnumerateDeviceExtensionProperties(vk_.physical_device, nullptr, &ext_count, nullptr);
	std::vector<VkExtensionProperties> dev_exts(ext_count);
	vk_.EnumerateDeviceExtensionProperties(vk_.physical_device, nullptr, &ext_count, dev_exts.data());
	auto dev_has = [&](const char *name) {
		for (const auto &e : dev_exts) {
			if (strcmp(e.extensionName, name) == 0) {
				return true;
			}
		}
		return false;
	};
	std::vector<const char *> enable;
	if (!dev_has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
		log("Vulkan: %s lacks " VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, props.deviceName);
		vk_.destroy();
		return false;
	}
	enable.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
	vk_.has_semaphore_fd = dev_has(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
	if (vk_.has_semaphore_fd) {
		enable.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
	}

	const float priority = 1.f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = vk_.queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = (uint32_t)enable.size();
	dci.ppEnabledExtensionNames = enable.data();
	XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	xdci.systemId = system_;
	xdci.pfnGetInstanceProcAddr = vk_.GetInstanceProcAddr;
	xdci.vulkanPhysicalDevice = vk_.physical_device;
	xdci.vulkanCreateInfo = &dci;
	if (!check(create_device(instance_, &xdci, &vk_.device, &vr_result), "xrCreateVulkanDeviceKHR") ||
	    vr_result != VK_SUCCESS) {
		log("Vulkan: device creation failed (VkResult %d)", (int)vr_result);
		vk_.device = VK_NULL_HANDLE;
		vk_.destroy();
		return false;
	}
	if (!vk_.load_device_functions()) {
		vk_.destroy();
		return false;
	}
	log("Vulkan: device %s, queue family %u, sync fd waits %s", props.deviceName, vk_.queue_family,
	    vk_.has_semaphore_fd ? "yes" : "no");
	return true;
}

bool
XrClient::create_swapchains()
{
	uint32_t count = 0;
	xrEnumerateSwapchainFormats(session_, 0, &count, nullptr);
	std::vector<int64_t> formats(count);
	if (!check(xrEnumerateSwapchainFormats(session_, count, &count, formats.data()), "xrEnumerateSwapchainFormats") ||
	    count == 0) {
		return false;
	}
	// sRGB like SteamVR's own textures, so the blit copies colours without conversion surprises.
	swapchain_format_ = formats[0];
	for (int64_t want : {(int64_t)VK_FORMAT_R8G8B8A8_SRGB, (int64_t)VK_FORMAT_B8G8R8A8_SRGB}) {
		bool found = false;
		for (int64_t f : formats) {
			found |= f == want;
		}
		if (found) {
			swapchain_format_ = want;
			break;
		}
	}

	if (!create_layer_swapchains(0)) {
		return false;
	}
	log("OpenXR: swapchains %ux%u, VkFormat %lld, %zu images", swapchain_width_[0], swapchain_height_[0],
	    (long long)swapchain_format_, swapchain_images_[0][0].size());
	return true;
}

bool
XrClient::create_layer_swapchains(int layer)
{
	for (int eye = 0; eye < 2; eye++) {
		XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		ci.format = swapchain_format_;
		ci.sampleCount = 1;
		ci.width = views_[eye].recommendedImageRectWidth;
		ci.height = views_[eye].recommendedImageRectHeight;
		ci.faceCount = 1;
		ci.arraySize = 1;
		ci.mipCount = 1;
		XrSwapchain &sc = swapchains_[layer][eye];
		if (!check(xrCreateSwapchain(session_, &ci, &sc), "xrCreateSwapchain")) {
			sc = XR_NULL_HANDLE;
			return false;
		}
		swapchain_width_[eye] = ci.width;
		swapchain_height_[eye] = ci.height;

		uint32_t n = 0;
		xrEnumerateSwapchainImages(sc, 0, &n, nullptr);
		std::vector<XrSwapchainImageVulkan2KHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
		if (!check(xrEnumerateSwapchainImages(sc, n, &n, (XrSwapchainImageBaseHeader *)images.data()),
		           "xrEnumerateSwapchainImages")) {
			return false;
		}
		swapchain_images_[layer][eye].clear();
		for (const auto &img : images) {
			swapchain_images_[layer][eye].push_back(img.image);
		}
	}
	return true;
}

bool
XrClient::ensure_layer(int layer)
{
	if (layer < 0 || layer >= k_max_layers) {
		return false;
	}
	if (!swapchain_images_[layer][0].empty() && !swapchain_images_[layer][1].empty()) {
		return true;
	}
	if (!vulkan_ || layer == 0 || !create_layer_swapchains(layer)) {
		return false;
	}
	log("OpenXR: swapchains for layer %d created", layer);
	return true;
}

bool
XrClient::wait_frame(XrTime *display_time, bool *should_render)
{
	XrFrameState fs{XR_TYPE_FRAME_STATE};
	XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
	if (!check(xrWaitFrame(session_, &wi, &fs), "xrWaitFrame")) {
		return false;
	}
	*display_time = fs.predictedDisplayTime;
	*should_render = fs.shouldRender == XR_TRUE;
	return true;
}

bool
XrClient::begin_frame()
{
	XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
	XrResult r = xrBeginFrame(session_, &bi);
	if (XR_FAILED(r)) { // XR_FRAME_DISCARDED is a success code: fine
		check(r, "xrBeginFrame");
		return false;
	}
	return true;
}

bool
XrClient::acquire(int layer, int eye, uint32_t *index)
{
	XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (!check(xrAcquireSwapchainImage(swapchains_[layer][eye], &ai, index), "xrAcquireSwapchainImage")) {
		return false;
	}
	XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = 100'000'000; // 100 ms
	XrResult r = xrWaitSwapchainImage(swapchains_[layer][eye], &wi);
	if (r != XR_SUCCESS) { // XR_TIMEOUT_EXPIRED is a success code but the image isn't ours
		check(r == XR_TIMEOUT_EXPIRED ? XR_ERROR_RUNTIME_FAILURE : r, "xrWaitSwapchainImage");
		release(layer, eye); // acquired: must be released (a release without a wait is allowed to fail)
		return false;
	}
	return *index < swapchain_images_[layer][eye].size();
}

void
XrClient::release(int layer, int eye)
{
	XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	xrReleaseSwapchainImage(swapchains_[layer][eye], &ri);
}

bool
XrClient::end_frame(XrTime display_time, const Layer *layers, int count)
{
	if (count > k_max_layers) {
		count = k_max_layers;
	}
	XrCompositionLayerProjectionView views[k_max_layers][2];
	XrCompositionLayerProjection projections[k_max_layers];
	const XrCompositionLayerBaseHeader *headers[k_max_layers];
	for (int l = 0; l < count; l++) {
		for (int eye = 0; eye < 2; eye++) {
			XrCompositionLayerProjectionView &v = views[l][eye];
			v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
			v.pose = layers[l].eye_poses[eye];
			v.fov = fov_[eye];
			v.subImage.swapchain = swapchains_[l][eye];
			v.subImage.imageRect = {{0, 0}, {(int32_t)swapchain_width_[eye], (int32_t)swapchain_height_[eye]}};
			v.subImage.imageArrayIndex = 0;
		}
		XrCompositionLayerProjection &p = projections[l];
		p = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		// Overlays (SteamVR's compositor layer: dashboard, ...) blend over what's below with their alpha,
		// which is straight, not premultiplied: in HL2:VR SteamVR always sends its compositor layer on
		// top of the game's, and with the dashboard closed its pixels are transparent but still carry
		// colour (a gradient). Blended as premultiplied (OpenXR's default) that colour covered the game.
		p.layerFlags = layers[l].blend ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
		                                     XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
		                               : 0;
		p.space = base_space_;
		p.viewCount = 2;
		p.views = views[l];
		headers[l] = (const XrCompositionLayerBaseHeader *)&p;
	}

	XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
	ei.displayTime = display_time;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = (uint32_t)count;
	ei.layers = count > 0 ? headers : nullptr;
	return check(xrEndFrame(session_, &ei), "xrEndFrame");
}

} // namespace ml
