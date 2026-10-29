// A minimal OpenXR runtime for the tests: just what XrClient uses, driven by FakeXrState.
// Loaded by the real OpenXR loader through build/fake_openxr_runtime.json.
#include "fake_openxr_state.h"

#include <ctime>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

extern "C" __attribute__((visibility("default"))) FakeXrState fake_xr_state;
FakeXrState fake_xr_state;

#define S fake_xr_state

static std::deque<XrEventDataBuffer> g_events;
static bool g_loss_sent = false;
static std::vector<std::string> g_paths = {""};         //!< XrPath = index
static std::map<uintptr_t, std::string> g_action_names; //!< action handle -> name
static uintptr_t g_next_action = 0x1000;

// Handles are small integers; a space's handle encodes its reference space type.
static XrInstance const k_instance = (XrInstance)(uintptr_t)0x1;
static XrSession const k_session = (XrSession)(uintptr_t)0x2;
static const XrSystemId k_system = 7;

static XrSpace
space_handle(XrReferenceSpaceType t)
{
	return (XrSpace)(uintptr_t)(0x100 + (uintptr_t)t);
}

static XrReferenceSpaceType
space_type(XrSpace s)
{
	return (XrReferenceSpaceType)((uintptr_t)s - 0x100);
}

static bool
lost()
{
	return S.lose_instance && g_loss_sent;
}

static void
push_state(XrSessionState state)
{
	XrEventDataBuffer buf{XR_TYPE_EVENT_DATA_BUFFER};
	auto *e = (XrEventDataSessionStateChanged *)&buf;
	e->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
	e->next = nullptr;
	e->session = k_session;
	e->state = state;
	e->time = 1;
	g_events.push_back(buf);
}

static const char *const k_extensions[] = {
    XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME,
    XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
    XR_MND_HEADLESS_EXTENSION_NAME,
};

static bool
extension_supported(const char *name)
{
	if (strcmp(name, XR_MND_HEADLESS_EXTENSION_NAME) == 0) {
		return S.support_headless;
	}
	if (strcmp(name, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) {
		return S.support_refresh_rate;
	}
	return strcmp(name, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME) == 0;
}

static XrResult XRAPI_CALL
f_EnumerateInstanceExtensionProperties(const char *layer, uint32_t cap, uint32_t *count, XrExtensionProperties *props)
{
	uint32_t n = 0;
	for (const char *name : k_extensions) {
		if (!extension_supported(name)) {
			continue;
		}
		if (cap > 0) {
			if (n >= cap) {
				return XR_ERROR_SIZE_INSUFFICIENT;
			}
			snprintf(props[n].extensionName, sizeof(props[n].extensionName), "%s", name);
			props[n].extensionVersion = 1;
		}
		n++;
	}
	*count = n;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_CreateInstance(const XrInstanceCreateInfo *info, XrInstance *out)
{
	for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
		if (!extension_supported(info->enabledExtensionNames[i])) {
			return XR_ERROR_EXTENSION_NOT_PRESENT;
		}
	}
	S.instances++;
	g_events.clear();
	g_loss_sent = false;
	g_action_names.clear();
	*out = k_instance;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_DestroyInstance(XrInstance)
{
	S.instances--;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_GetInstanceProperties(XrInstance, XrInstanceProperties *p)
{
	snprintf(p->runtimeName, sizeof(p->runtimeName), "monadolink fake runtime");
	p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_ResultToString(XrInstance, XrResult r, char buf[XR_MAX_RESULT_STRING_SIZE])
{
	snprintf(buf, XR_MAX_RESULT_STRING_SIZE, "XR_fake_result_%d", (int)r);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_StructureTypeToString(XrInstance, XrStructureType t, char buf[XR_MAX_STRUCTURE_NAME_SIZE])
{
	snprintf(buf, XR_MAX_STRUCTURE_NAME_SIZE, "XR_fake_type_%d", (int)t);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_GetSystem(XrInstance, const XrSystemGetInfo *info, XrSystemId *out)
{
	if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
		return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
	}
	*out = k_system;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_EnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType type, uint32_t cap,
                                  uint32_t *count, XrViewConfigurationView *views)
{
	if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	}
	*count = 2;
	if (cap == 0) {
		return XR_SUCCESS;
	}
	if (cap < 2) {
		return XR_ERROR_SIZE_INSUFFICIENT;
	}
	for (int i = 0; i < 2; i++) {
		views[i].recommendedImageRectWidth = S.width[i];
		views[i].recommendedImageRectHeight = S.height[i];
		views[i].maxImageRectWidth = 4096;
		views[i].maxImageRectHeight = 4096;
		views[i].recommendedSwapchainSampleCount = 1;
		views[i].maxSwapchainSampleCount = 1;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_CreateSession(XrInstance, const XrSessionCreateInfo *info, XrSession *out)
{
	S.session_had_graphics_binding = info->next != nullptr;
	if (info->next == nullptr && !S.support_headless) {
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	}
	S.sessions++;
	push_state(XR_SESSION_STATE_IDLE);
	push_state(XR_SESSION_STATE_READY);
	*out = k_session;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_DestroySession(XrSession)
{
	S.sessions--;
	S.session_begun = false;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_BeginSession(XrSession, const XrSessionBeginInfo *info)
{
	if (info->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
		return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
	}
	S.session_begun = true;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_EndSession(XrSession)
{
	S.session_begun = false;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_PollEvent(XrInstance, XrEventDataBuffer *out)
{
	if (S.lose_instance && !g_loss_sent) {
		g_loss_sent = true;
		*out = {XR_TYPE_EVENT_DATA_BUFFER};
		out->type = XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING;
		return XR_SUCCESS;
	}
	if (lost()) {
		return XR_ERROR_INSTANCE_LOST;
	}
	if (g_events.empty()) {
		return XR_EVENT_UNAVAILABLE;
	}
	*out = g_events.front();
	g_events.pop_front();
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_EnumerateReferenceSpaces(XrSession, uint32_t cap, uint32_t *count, XrReferenceSpaceType *types)
{
	XrReferenceSpaceType all[3] = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
	                               XR_REFERENCE_SPACE_TYPE_STAGE};
	const uint32_t n = S.support_stage ? 3 : 2;
	*count = n;
	if (cap == 0) {
		return XR_SUCCESS;
	}
	if (cap < n) {
		return XR_ERROR_SIZE_INSUFFICIENT;
	}
	memcpy(types, all, n * sizeof(all[0]));
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_CreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo *info, XrSpace *out)
{
	if (info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE && !S.support_stage) {
		return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
	}
	S.spaces++;
	*out = space_handle(info->referenceSpaceType);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_DestroySpace(XrSpace)
{
	S.spaces--;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_LocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *loc)
{
	if (lost()) {
		return XR_ERROR_INSTANCE_LOST;
	}
	const uintptr_t sv = (uintptr_t)space;
	const bool controller = sv == 0x2000 || sv == 0x2001;
	if (!controller && space_type(space) != XR_REFERENCE_SPACE_TYPE_VIEW) {
		return XR_ERROR_HANDLE_INVALID; // only the head and the grip spaces are supported
	}
	S.locate_calls++;
	S.last_locate_time = time;
	S.base_type = space_type(base);
	if (controller) {
		loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
		loc->pose = S.hands[sv - 0x2000].grip;
	} else {
		loc->locationFlags = S.head_flags;
		loc->pose = S.head;
	}
	for (auto *n = (XrBaseOutStructure *)loc->next; n != nullptr; n = n->next) {
		if (n->type == XR_TYPE_SPACE_VELOCITY) {
			auto *v = (XrSpaceVelocity *)n;
			v->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
			v->linearVelocity = S.linear_velocity;
			v->angularVelocity = S.angular_velocity;
		}
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_LocateViews(XrSession, const XrViewLocateInfo *info, XrViewState *state, uint32_t cap, uint32_t *count,
              XrView *views)
{
	*count = 2;
	if (cap == 0) {
		return XR_SUCCESS;
	}
	if (cap < 2) {
		return XR_ERROR_SIZE_INSUFFICIENT;
	}
	if (info->displayTime <= 0) {
		return XR_ERROR_TIME_INVALID;
	}
	state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
	for (int i = 0; i < 2; i++) {
		views[i].fov = S.fov[i];
		views[i].pose = {{0.f, 0.f, 0.f, 1.f}, {(i == 0 ? -0.5f : 0.5f) * S.ipd, 0.f, 0.f}};
		if (space_type(info->space) != XR_REFERENCE_SPACE_TYPE_VIEW) {
			views[i].pose.position.y += S.head.position.y;
		}
	}
	return XR_SUCCESS;
}

// --- Paths and actions ------------------------------------------------------------------------------

static XrResult XRAPI_CALL
f_StringToPath(XrInstance, const char *str, XrPath *out)
{
	for (size_t i = 1; i < g_paths.size(); i++) {
		if (g_paths[i] == str) {
			*out = i;
			return XR_SUCCESS;
		}
	}
	g_paths.push_back(str);
	*out = g_paths.size() - 1;
	return XR_SUCCESS;
}

static int
hand_of(XrPath p)
{
	if (p >= g_paths.size()) {
		return -1;
	}
	return g_paths[p] == "/user/hand/left" ? 0 : g_paths[p] == "/user/hand/right" ? 1 : -1;
}

static XrResult XRAPI_CALL
f_CreateActionSet(XrInstance, const XrActionSetCreateInfo *, XrActionSet *out)
{
	S.action_sets++;
	*out = (XrActionSet)(uintptr_t)0x500;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_DestroyActionSet(XrActionSet)
{
	S.action_sets--;
	S.actions = 0;
	g_action_names.clear();
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_CreateAction(XrActionSet, const XrActionCreateInfo *info, XrAction *out)
{
	S.actions++;
	uintptr_t h = g_next_action++;
	g_action_names[h] = info->actionName;
	*out = (XrAction)h;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_SuggestInteractionProfileBindings(XrInstance, const XrInteractionProfileSuggestedBinding *sb)
{
	S.suggest_calls++;
	for (uint32_t i = 0; i < sb->countSuggestedBindings; i++) {
		const std::string &path = g_paths[sb->suggestedBindings[i].binding];
		if (S.reject_optional_bindings &&
		    (path.find("thumbrest") != std::string::npos || path.find("menu") != std::string::npos)) {
			return XR_ERROR_PATH_UNSUPPORTED;
		}
	}
	S.suggested_bindings = (int)sb->countSuggestedBindings;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_AttachSessionActionSets(XrSession, const XrSessionActionSetsAttachInfo *)
{
	S.attached = true;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_CreateActionSpace(XrSession, const XrActionSpaceCreateInfo *info, XrSpace *out)
{
	const int hand = hand_of(info->subactionPath);
	if (hand < 0) {
		return XR_ERROR_PATH_INVALID;
	}
	S.spaces++;
	*out = (XrSpace)(uintptr_t)(0x2000 + hand);
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_SyncActions(XrSession, const XrActionsSyncInfo *)
{
	if (lost()) {
		return XR_ERROR_INSTANCE_LOST;
	}
	S.sync_calls++;
	return S.focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

//! The fake hand state for an action query, or null if inactive.
static const FakeXrState::Hand *
query(const XrActionStateGetInfo *gi, std::string *name)
{
	const int hand = hand_of(gi->subactionPath);
	auto it = g_action_names.find((uintptr_t)gi->action);
	if (hand < 0 || it == g_action_names.end() || !S.focused || !S.hands[hand].active) {
		return nullptr;
	}
	*name = it->second;
	return &S.hands[hand];
}

static XrResult XRAPI_CALL
f_GetActionStateBoolean(XrSession, const XrActionStateGetInfo *gi, XrActionStateBoolean *st)
{
	std::string n;
	const FakeXrState::Hand *h = query(gi, &n);
	st->isActive = h != nullptr;
	st->currentState = XR_FALSE;
	if (h != nullptr) {
		const bool v = n == "trigger_touch" ? h->trigger_touch
		               : n == "thumbstick_click" ? h->stick_click
		               : n == "thumbstick_touch" ? h->stick_touch
		               : n == "thumbrest_touch" ? h->thumbrest_touch
		               : n == "lower_button_click" ? h->lower_click
		               : n == "lower_button_touch" ? h->lower_touch
		               : n == "upper_button_click" ? h->upper_click
		               : n == "upper_button_touch" ? h->upper_touch
		               : n == "menu_click" ? h->menu_click
		                                   : false;
		st->currentState = v ? XR_TRUE : XR_FALSE;
	}
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_GetActionStateFloat(XrSession, const XrActionStateGetInfo *gi, XrActionStateFloat *st)
{
	std::string n;
	const FakeXrState::Hand *h = query(gi, &n);
	st->isActive = h != nullptr;
	st->currentState = h == nullptr ? 0.f : n == "trigger" ? h->trigger : n == "squeeze" ? h->squeeze : 0.f;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_GetActionStateVector2f(XrSession, const XrActionStateGetInfo *gi, XrActionStateVector2f *st)
{
	std::string n;
	const FakeXrState::Hand *h = query(gi, &n);
	st->isActive = h != nullptr;
	st->currentState = h == nullptr ? XrVector2f{0.f, 0.f} : XrVector2f{h->stick_x, h->stick_y};
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_GetActionStatePose(XrSession, const XrActionStateGetInfo *gi, XrActionStatePose *st)
{
	std::string n;
	st->isActive = query(gi, &n) != nullptr;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_ApplyHapticFeedback(XrSession, const XrHapticActionInfo *info, const XrHapticBaseHeader *h)
{
	const auto *v = (const XrHapticVibration *)h;
	S.haptic_calls++;
	S.haptic_hand = hand_of(info->subactionPath);
	S.haptic_amplitude = v->amplitude;
	S.haptic_frequency = v->frequency;
	S.haptic_duration = v->duration;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_ConvertTimespecTimeToTimeKHR(XrInstance, const struct timespec *ts, XrTime *out)
{
	*out = (XrTime)ts->tv_sec * 1000000000 + ts->tv_nsec;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_GetDisplayRefreshRateFB(XrSession, float *rate)
{
	*rate = S.refresh_rate;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL
f_GetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *out)
{
#define ENTRY(fn)                                                                                                      \
	if (strcmp(name, "xr" #fn) == 0) {                                                                             \
		*out = (PFN_xrVoidFunction)f_##fn;                                                                     \
		return XR_SUCCESS;                                                                                     \
	}
	ENTRY(GetInstanceProcAddr)
	ENTRY(EnumerateInstanceExtensionProperties)
	ENTRY(CreateInstance)
	if (instance != XR_NULL_HANDLE) {
		ENTRY(DestroyInstance)
		ENTRY(GetInstanceProperties)
		ENTRY(ResultToString)
		ENTRY(StructureTypeToString)
		ENTRY(GetSystem)
		ENTRY(EnumerateViewConfigurationViews)
		ENTRY(CreateSession)
		ENTRY(DestroySession)
		ENTRY(BeginSession)
		ENTRY(EndSession)
		ENTRY(PollEvent)
		ENTRY(EnumerateReferenceSpaces)
		ENTRY(CreateReferenceSpace)
		ENTRY(DestroySpace)
		ENTRY(LocateSpace)
		ENTRY(LocateViews)
		ENTRY(ConvertTimespecTimeToTimeKHR)
		ENTRY(StringToPath)
		ENTRY(CreateActionSet)
		ENTRY(DestroyActionSet)
		ENTRY(CreateAction)
		ENTRY(SuggestInteractionProfileBindings)
		ENTRY(AttachSessionActionSets)
		ENTRY(CreateActionSpace)
		ENTRY(SyncActions)
		ENTRY(GetActionStateBoolean)
		ENTRY(GetActionStateFloat)
		ENTRY(GetActionStateVector2f)
		ENTRY(GetActionStatePose)
		ENTRY(ApplyHapticFeedback)
		ENTRY(GetDisplayRefreshRateFB)
	}
#undef ENTRY
	*out = nullptr;
	return XR_ERROR_FUNCTION_UNSUPPORTED;
}

extern "C" __attribute__((visibility("default"))) XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *loader, XrNegotiateRuntimeRequest *request)
{
	if (loader == nullptr || request == nullptr || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
	    request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST) {
		return XR_ERROR_INITIALIZATION_FAILED;
	}
	request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	request->runtimeApiVersion = XR_CURRENT_API_VERSION;
	request->getInstanceProcAddr = f_GetInstanceProcAddr;
	return XR_SUCCESS;
}
