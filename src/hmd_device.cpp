#include "hmd_device.h"

#include "util/log.h"
#include "xr/xr_math.h"

#include <cmath>
#include <cstring>

namespace ml {

static const char *k_main_section = "driver_monadolink";
static const char *k_display_section = "monadolink_display";

DisplayConfig
default_display_config()
{
	DisplayConfig c{};
	c.render_width = (uint32_t)vr::VRSettings()->GetInt32(k_display_section, "render_width");
	c.render_height = (uint32_t)vr::VRSettings()->GetInt32(k_display_section, "render_height");
	c.refresh_rate = vr::VRSettings()->GetFloat(k_display_section, "refresh_rate");
	c.seconds_from_vsync_to_photons = vr::VRSettings()->GetFloat(k_display_section, "seconds_from_vsync_to_photons");

	// Placeholder used without Monado: an approximate Quest 2 field of view (degrees: 52 outward,
	// 42 inward, 48 up, 50 down). XrClient::apply_display_info replaces it with xrLocateViews.
	auto t = [](float deg) { return std::tan(deg * (float)M_PI / 180.f); };
	for (int eye = 0; eye < 2; eye++) {
		const float outward = t(52.f), inward = t(42.f);
		c.tan_left[eye] = eye == 0 ? -outward : -inward;
		c.tan_right[eye] = eye == 0 ? inward : outward;
		c.tan_top[eye] = -t(50.f); // OpenVR raw: top = tan(down edge), bottom = tan(up edge)
		c.tan_bottom[eye] = t(48.f);
	}
	c.ipd_meters = vr::VRSettings()->GetFloat(vr::k_pch_SteamVR_Section, vr::k_pch_SteamVR_IPD_Float);
	c.has_eye_to_head = false;
	return c;
}

static std::string
get_setting_string(const char *section, const char *key)
{
	char buf[256] = {};
	vr::VRSettings()->GetString(section, key, buf, sizeof(buf));
	return buf;
}

// --- DisplayComponent ---------------------------------------------------------------------------

void
DisplayComponent::GetWindowBounds(int32_t *x, int32_t *y, uint32_t *width, uint32_t *height)
{
	std::lock_guard lock(mutex_);
	*x = 0;
	*y = 0;
	*width = config_.render_width * 2;
	*height = config_.render_height;
}

void
DisplayComponent::GetRecommendedRenderTargetSize(uint32_t *width, uint32_t *height)
{
	std::lock_guard lock(mutex_);
	*width = config_.render_width;
	*height = config_.render_height;
}

void
DisplayComponent::GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *width, uint32_t *height)
{
	std::lock_guard lock(mutex_);
	*x = eye == vr::Eye_Left ? 0 : config_.render_width;
	*y = 0;
	*width = config_.render_width;
	*height = config_.render_height;
}

void
DisplayComponent::GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top, float *bottom)
{
	std::lock_guard lock(mutex_);
	*left = config_.tan_left[eye];
	*right = config_.tan_right[eye];
	*top = config_.tan_top[eye];
	*bottom = config_.tan_bottom[eye];
}

vr::DistortionCoordinates_t
DisplayComponent::ComputeDistortion(vr::EVREye eye, float u, float v)
{
	// No lens distortion here: Monado's compositor applies the headset's own distortion.
	vr::DistortionCoordinates_t c{};
	c.rfRed[0] = c.rfGreen[0] = c.rfBlue[0] = u;
	c.rfRed[1] = c.rfGreen[1] = c.rfBlue[1] = v;
	return c;
}

// --- HmdDevice ----------------------------------------------------------------------------------

static vr::DriverPose_t
static_pose()
{
	// Without Monado: a static head 1.6 m above the floor, looking forward.
	vr::DriverPose_t pose{};
	pose.qWorldFromDriverRotation.w = 1.0;
	pose.qDriverFromHeadRotation.w = 1.0;
	pose.qRotation.w = 1.0;
	pose.vecPosition[1] = 1.6;
	pose.poseIsValid = true;
	pose.deviceIsConnected = true;
	pose.result = vr::TrackingResult_Running_OK;
	return pose;
}

HmdDevice::HmdDevice(const DisplayConfig &display, XrClient *xr)
    : pose_(static_pose()), serial_(get_setting_string(k_main_section, "serial_number")),
      model_(get_setting_string(k_main_section, "model_number")), display_config_(display), display_(display_config_),
      direct_mode_(display_config_.refresh_rate, vr::VRSettings()->GetInt32(k_main_section, "log_frames"), xr,
                   vr::VRSettings()->GetBool(k_main_section, "early_wait"),
                   vr::VRSettings()->GetBool(k_main_section, "overlay_layers"))
{
	direct_mode_.set_overlay_debug(vr::VRSettings()->GetInt32(k_main_section, "overlay_debug"));
	direct_mode_.set_upscale(display.fsr_scale < 1.f, display.fsr_sharpness);
}

vr::EVRInitError
HmdDevice::Activate(uint32_t object_id)
{
	device_index_ = object_id;
	log("HMD activated as device %u (%s): %ux%u per eye @ %.0f Hz", object_id, process_tag(),
	    display_config_.render_width, display_config_.render_height, display_config_.refresh_rate);

	vr::PropertyContainerHandle_t c = vr::VRProperties()->TrackedDeviceToPropertyContainer(device_index_);
	vr::VRProperties()->SetStringProperty(c, vr::Prop_ModelNumber_String, model_.c_str());
	vr::VRProperties()->SetStringProperty(c, vr::Prop_ManufacturerName_String, "Monado");
	vr::VRProperties()->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "monadolink");
	vr::VRProperties()->SetStringProperty(c, vr::Prop_InputProfilePath_String,
	                                      "{monadolink}/input/monadolink_hmd_profile.json");

	vr::VRProperties()->SetFloatProperty(c, vr::Prop_DisplayFrequency_Float, display_config_.refresh_rate);
	vr::VRProperties()->SetFloatProperty(c, vr::Prop_SecondsFromVsyncToPhotons_Float,
	                                     display_config_.seconds_from_vsync_to_photons);
	vr::VRProperties()->SetFloatProperty(c, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.f);
	vr::VRProperties()->SetBoolProperty(c, vr::Prop_IsOnDesktop_Bool, false);

	// Frames come to us through IVRDriverDirectModeComponent instead of a real display.
	vr::VRProperties()->SetBoolProperty(c, vr::Prop_HasDriverDirectModeComponent_Bool, true);
	vr::VRProperties()->SetBoolProperty(c, vr::Prop_DisplayDebugMode_Bool, false);

	{
		std::lock_guard lock(eyes_mutex_);
		vr::VRProperties()->SetFloatProperty(c, vr::Prop_UserIpdMeters_Float, display_config_.ipd_meters);
		if (display_config_.has_eye_to_head) {
			vr::VRServerDriverHost()->SetDisplayEyeToHead(object_id, display_config_.eye_to_head[0],
			                                              display_config_.eye_to_head[1]);
		}
	}
	return vr::VRInitError_None;
}

void
HmdDevice::Deactivate()
{
	log("HMD deactivated");
	device_index_ = vr::k_unTrackedDeviceIndexInvalid;
}

void *
HmdDevice::GetComponent(const char *component_name_and_version)
{
	if (strcmp(component_name_and_version, vr::IVRDisplayComponent_Version) == 0) {
		return static_cast<vr::IVRDisplayComponent *>(&display_);
	}
	if (strcmp(component_name_and_version, vr::IVRDriverDirectModeComponent_Version) == 0) {
		log("Direct-mode component requested (%s)", process_tag());
		return static_cast<vr::IVRDriverDirectModeComponent *>(&direct_mode_);
	}
	return nullptr;
}

void
HmdDevice::DebugRequest(const char *request, char *response, uint32_t response_size)
{
	if (response_size > 0) {
		response[0] = '\0';
	}
}

vr::DriverPose_t
HmdDevice::GetPose()
{
	std::lock_guard lock(pose_mutex_);
	return pose_;
}

void
HmdDevice::update_pose(const vr::DriverPose_t &pose)
{
	{
		std::lock_guard lock(pose_mutex_);
		pose_ = pose;
	}
	const uint32_t index = device_index_;
	if (index != vr::k_unTrackedDeviceIndexInvalid) {
		vr::VRServerDriverHost()->TrackedDevicePoseUpdated(index, pose, sizeof(vr::DriverPose_t));
	}
}

void
HmdDevice::update_eyes(const XrPosef eyes[2])
{
	float ipd = 0.f;
	plausible_ipd(eyes, &ipd);
	std::lock_guard lock(eyes_mutex_); // also keeps Activate from sending older poses after these
	display_config_.ipd_meters = ipd;
	display_config_.eye_to_head[0] = matrix_from_xr_pose(eyes[0]);
	display_config_.eye_to_head[1] = matrix_from_xr_pose(eyes[1]);
	display_config_.has_eye_to_head = true;
	const uint32_t index = device_index_;
	if (index == vr::k_unTrackedDeviceIndexInvalid) {
		return; // Activate sends them
	}
	vr::VRServerDriverHost()->SetDisplayEyeToHead(index, display_config_.eye_to_head[0], display_config_.eye_to_head[1]);
	vr::VRProperties()->SetFloatProperty(vr::VRProperties()->TrackedDeviceToPropertyContainer(index),
	                                     vr::Prop_UserIpdMeters_Float, ipd);
}

void
HmdDevice::update_display(const DisplayConfig &display)
{
	{
		std::lock_guard lock(eyes_mutex_);
		const bool ipd_known = display_config_.has_eye_to_head;
		const DisplayConfig keep = display_config_;
		display_config_ = display;
		if (ipd_known && !display.has_eye_to_head) { // update_eyes may have come first
			display_config_.has_eye_to_head = true;
			display_config_.ipd_meters = keep.ipd_meters;
			display_config_.eye_to_head[0] = keep.eye_to_head[0];
			display_config_.eye_to_head[1] = keep.eye_to_head[1];
		}
	}
	display_.set(display_config_);
	direct_mode_.set_projection_enforced(true);
	const uint32_t index = device_index_;
	if (index == vr::k_unTrackedDeviceIndexInvalid) {
		return; // Activate sends them
	}
	vr::HmdRect2_t eye[2];
	for (int e = 0; e < 2; e++) {
		eye[e].vTopLeft = {display.tan_left[e], display.tan_top[e]};
		eye[e].vBottomRight = {display.tan_right[e], display.tan_bottom[e]};
	}
	vr::VRServerDriverHost()->SetDisplayProjectionRaw(index, eye[0], eye[1]);
	vr::VRServerDriverHost()->SetRecommendedRenderTargetSize(index, display.render_width, display.render_height);
	vr::PropertyContainerHandle_t c = vr::VRProperties()->TrackedDeviceToPropertyContainer(index);
	vr::VRProperties()->SetFloatProperty(c, vr::Prop_DisplayFrequency_Float, display.refresh_rate);
	std::lock_guard lock(eyes_mutex_);
	if (display_config_.has_eye_to_head) {
		vr::VRServerDriverHost()->SetDisplayEyeToHead(index, display_config_.eye_to_head[0], display_config_.eye_to_head[1]);
		vr::VRProperties()->SetFloatProperty(c, vr::Prop_UserIpdMeters_Float, display_config_.ipd_meters);
	}
	log("HMD display updated from Monado: %ux%u per eye @ %.0f Hz, left eye tan l/r/t/b %.3f/%.3f/%.3f/%.3f",
	    display.render_width, display.render_height, display.refresh_rate, display.tan_left[0], display.tan_right[0],
	    display.tan_top[0], display.tan_bottom[0]);
}

void
HmdDevice::run_frame()
{
	update_pose(GetPose());
}

} // namespace ml
