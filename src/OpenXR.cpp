// Tell OpenXR what platform code we'll be using
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include "openxr.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

// Forward declarations for functions/objects provided by main.cpp (D3D + App)
extern bool                 d3d_init(LUID& adapter_luid);
extern void                 d3d_swapchain_destroy(swapchain_t& swapchain);
extern swapchain_surfdata_t d3d_make_surface_data(XrBaseInStructure& swapchainImage);
extern void                 d3d_render_layer(XrCompositionLayerProjectionView& layerView, swapchain_surfdata_t& surface);

// D3D globals provided by main.cpp
extern ID3D11Device* d3d_device;
extern int64_t       d3d_swapchain_fmt;

// App hooks provided by main.cpp
extern void app_update_predicted();

// Function pointers for OpenXR extension methods
static PFN_xrGetD3D11GraphicsRequirementsKHR ext_xrGetD3D11GraphicsRequirementsKHR = nullptr;
static PFN_xrCreateDebugUtilsMessengerEXT    ext_xrCreateDebugUtilsMessengerEXT = nullptr;
static PFN_xrDestroyDebugUtilsMessengerEXT   ext_xrDestroyDebugUtilsMessengerEXT = nullptr;

// OpenXR state (internal except the ones exposed in header)
const XrPosef  xr_pose_identity = { {0,0,0,1}, {0,0,0} };
static XrInstance     xr_instance = {};
static XrSession      xr_session = {};
XrSessionState        xr_session_state = XR_SESSION_STATE_UNKNOWN;
bool                  xr_running = false;
static XrSpace        xr_app_space = {};
static XrSystemId     xr_system_id = XR_NULL_SYSTEM_ID;
input_state_t         xr_input = { };
static XrEnvironmentBlendMode   xr_blend = {};
static XrDebugUtilsMessengerEXT xr_debug = {};

static std::vector<XrView>                  xr_views;
static std::vector<XrViewConfigurationView> xr_config_views;
static std::vector<swapchain_t>             xr_swapchains;

static XrFormFactor            app_config_form = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
static XrViewConfigurationType app_config_view = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

bool openxr_init(const char* app_name, int64_t swapchain_format) {
	std::vector<const char*> use_extensions;
	const char* ask_extensions[] = {
		XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
		XR_EXT_DEBUG_UTILS_EXTENSION_NAME,
	};

	uint32_t ext_count = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
	std::vector<XrExtensionProperties> xr_exts(ext_count, { XR_TYPE_EXTENSION_PROPERTIES });
	xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, xr_exts.data());

	printf("OpenXR extensions available:\n");
	for (size_t i = 0; i < xr_exts.size(); i++) {
		printf("- %s\n", xr_exts[i].extensionName);
		for (int32_t ask = 0; ask < (int32_t)(_countof(ask_extensions)); ask++) {
			if (strcmp(ask_extensions[ask], xr_exts[i].extensionName) == 0) {
				use_extensions.push_back(ask_extensions[ask]);
				break;
			}
		}
	}
	if (!std::any_of(use_extensions.begin(), use_extensions.end(),
		[](const char* ext) { return strcmp(ext, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0; }))
		return false;

	XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
	createInfo.enabledExtensionCount = (uint32_t)use_extensions.size();
	createInfo.enabledExtensionNames = use_extensions.data();
	createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	strcpy_s(createInfo.applicationInfo.applicationName, app_name);
	xrCreateInstance(&createInfo, &xr_instance);
	if (xr_instance == nullptr)
		return false;

	xrGetInstanceProcAddr(xr_instance, "xrCreateDebugUtilsMessengerEXT", (PFN_xrVoidFunction*)(&ext_xrCreateDebugUtilsMessengerEXT));
	xrGetInstanceProcAddr(xr_instance, "xrDestroyDebugUtilsMessengerEXT", (PFN_xrVoidFunction*)(&ext_xrDestroyDebugUtilsMessengerEXT));
	xrGetInstanceProcAddr(xr_instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&ext_xrGetD3D11GraphicsRequirementsKHR));

	XrDebugUtilsMessengerCreateInfoEXT debug_info = { XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
	debug_info.messageTypes =
		XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
		XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT;
	debug_info.messageSeverities =
		XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
		XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
		XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	debug_info.userCallback = [](XrDebugUtilsMessageSeverityFlagsEXT, XrDebugUtilsMessageTypeFlagsEXT, const XrDebugUtilsMessengerCallbackDataEXT* msg, void*) {
		printf("%s: %s\n", msg->functionName, msg->message);
		char text[512];
		sprintf_s(text, "%s: %s", msg->functionName, msg->message);
		OutputDebugStringA(text);
		return (XrBool32)XR_FALSE;
		};
	if (ext_xrCreateDebugUtilsMessengerEXT)
		ext_xrCreateDebugUtilsMessengerEXT(xr_instance, &debug_info, &xr_debug);

	XrSystemGetInfo systemInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = app_config_form;
	xrGetSystem(xr_instance, &systemInfo, &xr_system_id);

	uint32_t blend_count = 0;
	xrEnumerateEnvironmentBlendModes(xr_instance, xr_system_id, app_config_view, 1, &blend_count, &xr_blend);

	XrGraphicsRequirementsD3D11KHR requirement = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
	ext_xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system_id, &requirement);
	if (!d3d_init(requirement.adapterLuid))
		return false;

	XrGraphicsBindingD3D11KHR binding = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	binding.device = d3d_device;
	XrSessionCreateInfo sessionInfo = { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = xr_system_id;
	xrCreateSession(xr_instance, &sessionInfo, &xr_session);
	if (xr_session == nullptr)
		return false;

	XrReferenceSpaceCreateInfo ref_space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	ref_space.poseInReferenceSpace = xr_pose_identity;
	ref_space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	xrCreateReferenceSpace(xr_session, &ref_space, &xr_app_space);

	uint32_t view_count = 0;
	xrEnumerateViewConfigurationViews(xr_instance, xr_system_id, app_config_view, 0, &view_count, nullptr);
	xr_config_views.resize(view_count, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
	xr_views.resize(view_count, { XR_TYPE_VIEW });
	xrEnumerateViewConfigurationViews(xr_instance, xr_system_id, app_config_view, view_count, &view_count, xr_config_views.data());
	for (uint32_t i = 0; i < view_count; i++) {
		XrViewConfigurationView& view = xr_config_views[i];
		XrSwapchainCreateInfo    swapchain_info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		XrSwapchain              handle;
		swapchain_info.arraySize = 1;
		swapchain_info.mipCount = 1;
		swapchain_info.faceCount = 1;
		swapchain_info.format = (int64_t)swapchain_format;
		swapchain_info.width = view.recommendedImageRectWidth;
		swapchain_info.height = view.recommendedImageRectHeight;
		swapchain_info.sampleCount = view.recommendedSwapchainSampleCount;
		swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		xrCreateSwapchain(xr_session, &swapchain_info, &handle);

		uint32_t surface_count = 0;
		xrEnumerateSwapchainImages(handle, 0, &surface_count, nullptr);

		swapchain_t swapchain = {};
		swapchain.width = swapchain_info.width;
		swapchain.height = swapchain_info.height;
		swapchain.handle = handle;
		swapchain.surface_images.resize(surface_count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		swapchain.surface_data.resize(surface_count);
		xrEnumerateSwapchainImages(swapchain.handle, surface_count, &surface_count, (XrSwapchainImageBaseHeader*)swapchain.surface_images.data());
		for (uint32_t j = 0; j < surface_count; j++) {
			swapchain.surface_data[j] = d3d_make_surface_data((XrBaseInStructure&)swapchain.surface_images[j]);
		}
		xr_swapchains.push_back(swapchain);
	}

	return true;
}



void openxr_shutdown() {
	for (size_t i = 0; i < xr_swapchains.size(); i++) {
		xrDestroySwapchain(xr_swapchains[i].handle);
		d3d_swapchain_destroy(xr_swapchains[i]);
	}
	xr_swapchains.clear();

	if (xr_input.actionSet != XR_NULL_HANDLE) {
		if (xr_input.handSpace[0] != XR_NULL_HANDLE) xrDestroySpace(xr_input.handSpace[0]);
		if (xr_input.handSpace[1] != XR_NULL_HANDLE) xrDestroySpace(xr_input.handSpace[1]);
		xrDestroyActionSet(xr_input.actionSet);
	}
	if (xr_app_space != XR_NULL_HANDLE) xrDestroySpace(xr_app_space);
	if (xr_session != XR_NULL_HANDLE) xrDestroySession(xr_session);
	if (xr_debug != XR_NULL_HANDLE && ext_xrDestroyDebugUtilsMessengerEXT) ext_xrDestroyDebugUtilsMessengerEXT(xr_debug);
	if (xr_instance != XR_NULL_HANDLE) xrDestroyInstance(xr_instance);
}

void openxr_poll_events(bool& exit) {
	exit = false;

	XrEventDataBuffer event_buffer = { XR_TYPE_EVENT_DATA_BUFFER };

	while (xrPollEvent(xr_instance, &event_buffer) == XR_SUCCESS) {
		switch (event_buffer.type) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
			XrEventDataSessionStateChanged* changed = (XrEventDataSessionStateChanged*)&event_buffer;
			xr_session_state = changed->state;

			switch (xr_session_state) {
			case XR_SESSION_STATE_READY: {
				XrSessionBeginInfo begin_info = { XR_TYPE_SESSION_BEGIN_INFO };
				begin_info.primaryViewConfigurationType = app_config_view;
				xrBeginSession(xr_session, &begin_info);
				xr_running = true;
			} break;
			case XR_SESSION_STATE_STOPPING: {
				xr_running = false;
				xrEndSession(xr_session);
			} break;
			case XR_SESSION_STATE_EXITING:      exit = true;              break;
			case XR_SESSION_STATE_LOSS_PENDING: exit = true;              break;
			default: break;
			}
		} break;
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: exit = true; return;
		default: break;
		}
		event_buffer = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

void openxr_make_actions() {
	XrActionSetCreateInfo actionset_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(actionset_info.actionSetName, "gameplay");
	strcpy_s(actionset_info.localizedActionSetName, "Gameplay");
	xrCreateActionSet(xr_instance, &actionset_info, &xr_input.actionSet);
	xrStringToPath(xr_instance, "/user/hand/left", &xr_input.handSubactionPath[0]);
	xrStringToPath(xr_instance, "/user/hand/right", &xr_input.handSubactionPath[1]);

	// Pose action
	XrActionCreateInfo action_info = { XR_TYPE_ACTION_CREATE_INFO };
	action_info.countSubactionPaths = _countof(xr_input.handSubactionPath);
	action_info.subactionPaths = xr_input.handSubactionPath;
	action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
	strcpy_s(action_info.actionName, "hand_pose");
	strcpy_s(action_info.localizedActionName, "Hand Pose");
	xrCreateAction(xr_input.actionSet, &action_info, &xr_input.poseAction);

	// Select (trigger)
	action_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
	strcpy_s(action_info.actionName, "select");
	strcpy_s(action_info.localizedActionName, "Select");
	xrCreateAction(xr_input.actionSet, &action_info, &xr_input.selectAction);

	// Secondary (menu/B) for right-click
	action_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
	strcpy_s(action_info.actionName, "secondary");
	strcpy_s(action_info.localizedActionName, "Secondary");
	xrCreateAction(xr_input.actionSet, &action_info, &xr_input.secondaryAction);

	// ---- Bindings: Khronos simple_controller (fallback) ----
	{
		XrPath profile_path;
		xrStringToPath(xr_instance, "/interaction_profiles/khr/simple_controller", &profile_path);

		XrPath pose_path[2], select_path[2], menu_path[2];
		xrStringToPath(xr_instance, "/user/hand/left/input/grip/pose", &pose_path[0]);
		xrStringToPath(xr_instance, "/user/hand/right/input/grip/pose", &pose_path[1]);
		xrStringToPath(xr_instance, "/user/hand/left/input/select/click", &select_path[0]);
		xrStringToPath(xr_instance, "/user/hand/right/input/select/click", &select_path[1]);
		xrStringToPath(xr_instance, "/user/hand/left/input/menu/click", &menu_path[0]);
		xrStringToPath(xr_instance, "/user/hand/right/input/menu/click", &menu_path[1]);

		XrActionSuggestedBinding bindings[] = {
			{ xr_input.poseAction,      pose_path[0] },
			{ xr_input.poseAction,      pose_path[1] },
			{ xr_input.selectAction,    select_path[0] },
			{ xr_input.selectAction,    select_path[1] },
			{ xr_input.secondaryAction, menu_path[0] },
			{ xr_input.secondaryAction, menu_path[1] },
		};
		XrInteractionProfileSuggestedBinding s{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		s.interactionProfile = profile_path;
		s.suggestedBindings = bindings;
		s.countSuggestedBindings = (uint32_t)_countof(bindings);
		xrSuggestInteractionProfileBindings(xr_instance, &s);
	}

	// ---- Bindings: Oculus Touch (Quest/Link) ----
	// Provide ALL needed bindings (pose + select + secondary) for this profile too.
	{
		XrPath profile_path;
		if (XR_SUCCEEDED(xrStringToPath(xr_instance, "/interaction_profiles/oculus/touch_controller", &profile_path))) {
			XrPath poseL, poseR;
			XrPath trigL, trigR;           // trigger/click (boolean)
			XrPath btnB, btnX;             // B (right), X (left) for secondary

			xrStringToPath(xr_instance, "/user/hand/left/input/grip/pose", &poseL);
			xrStringToPath(xr_instance, "/user/hand/right/input/grip/pose", &poseR);

			// Some runtimes expose trigger as click for boolean actions
			xrStringToPath(xr_instance, "/user/hand/left/input/trigger/click", &trigL);
			xrStringToPath(xr_instance, "/user/hand/right/input/trigger/click", &trigR);

			xrStringToPath(xr_instance, "/user/hand/right/input/b/click", &btnB);
			xrStringToPath(xr_instance, "/user/hand/left/input/x/click", &btnX);

			XrActionSuggestedBinding bindings[] = {
				{ xr_input.poseAction,      poseL },
				{ xr_input.poseAction,      poseR },
				{ xr_input.selectAction,    trigL },
				{ xr_input.selectAction,    trigR },
				{ xr_input.secondaryAction, btnB  },
				{ xr_input.secondaryAction, btnX  },
			};
			XrInteractionProfileSuggestedBinding s{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
			s.interactionProfile = profile_path;
			s.suggestedBindings = bindings;
			s.countSuggestedBindings = (uint32_t)_countof(bindings);
			xrSuggestInteractionProfileBindings(xr_instance, &s);
		}
	}

	// Create action spaces for the hand poses
	for (int32_t i = 0; i < 2; i++) {
		XrActionSpaceCreateInfo action_space_info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		action_space_info.action = xr_input.poseAction;
		action_space_info.poseInActionSpace = xr_pose_identity;
		action_space_info.subactionPath = xr_input.handSubactionPath[i];
		xrCreateActionSpace(xr_session, &action_space_info, &xr_input.handSpace[i]);
	}

	// Attach the action set
	XrSessionActionSetsAttachInfo attach_info = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach_info.countActionSets = 1;
	attach_info.actionSets = &xr_input.actionSet;
	xrAttachSessionActionSets(xr_session, &attach_info);
}

void openxr_poll_actions() {
	if (xr_session_state != XR_SESSION_STATE_FOCUSED)
		return;

	XrActiveActionSet action_set = {};
	action_set.actionSet = xr_input.actionSet;
	action_set.subactionPath = XR_NULL_PATH;

	XrActionsSyncInfo sync_info = { XR_TYPE_ACTIONS_SYNC_INFO };
	sync_info.countActiveActionSets = 1;
	sync_info.activeActionSets = &action_set;
	xrSyncActions(xr_session, &sync_info);

	for (uint32_t hand = 0; hand < 2; hand++) {
		XrActionStateGetInfo get_info = { XR_TYPE_ACTION_STATE_GET_INFO };
		get_info.subactionPath = xr_input.handSubactionPath[hand];

		// Pose active?
		XrActionStatePose pose_state = { XR_TYPE_ACTION_STATE_POSE };
		get_info.action = xr_input.poseAction;
		xrGetActionStatePose(xr_session, &get_info, &pose_state);
		xr_input.renderHand[hand] = pose_state.isActive;

		// Select (trigger) edge
		XrActionStateBoolean select_state = { XR_TYPE_ACTION_STATE_BOOLEAN };
		get_info.action = xr_input.selectAction;
		xrGetActionStateBoolean(xr_session, &get_info, &select_state);
		const bool selectEdge = (!!select_state.currentState) && (!!select_state.changedSinceLastSync);
		xr_input.handSelect[hand] = selectEdge ? XR_TRUE : XR_FALSE;

		// Secondary (B/menu) edge
		XrActionStateBoolean sec_state = { XR_TYPE_ACTION_STATE_BOOLEAN };
		get_info.action = xr_input.secondaryAction;
		xrGetActionStateBoolean(xr_session, &get_info, &sec_state);
		const bool secondaryEdge = (!!sec_state.currentState) && (!!sec_state.changedSinceLastSync);
		xr_input.handSecondary[hand] = secondaryEdge ? XR_TRUE : XR_FALSE;

		// If an edge occurred, sample pose at that exact timestamp
		if (selectEdge) {
			XrSpaceLocation space_location = { XR_TYPE_SPACE_LOCATION };
			XrResult res = xrLocateSpace(xr_input.handSpace[hand], xr_app_space, select_state.lastChangeTime, &space_location);
			if (XR_UNQUALIFIED_SUCCESS(res) &&
				(space_location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
				(space_location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
				xr_input.handPose[hand] = space_location.pose;
			}
		}
		else if (secondaryEdge) {
			XrSpaceLocation space_location = { XR_TYPE_SPACE_LOCATION };
			XrResult res = xrLocateSpace(xr_input.handSpace[hand], xr_app_space, sec_state.lastChangeTime, &space_location);
			if (XR_UNQUALIFIED_SUCCESS(res) &&
				(space_location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
				(space_location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
				xr_input.handPose[hand] = space_location.pose;
			}
		}
	}
}




void openxr_poll_predicted(XrTime predicted_time) {
	if (xr_session_state != XR_SESSION_STATE_FOCUSED)
		return;

	for (size_t i = 0; i < 2; i++) {
		if (!xr_input.renderHand[i])
			continue;
		XrSpaceLocation spaceRelation = { XR_TYPE_SPACE_LOCATION };
		XrResult        res = xrLocateSpace(xr_input.handSpace[i], xr_app_space, predicted_time, &spaceRelation);
		if (XR_UNQUALIFIED_SUCCESS(res) &&
			(spaceRelation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
			(spaceRelation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
			xr_input.handPose[i] = spaceRelation.pose;
		}
	}
}

void openxr_render_frame() {
	XrFrameState frame_state = { XR_TYPE_FRAME_STATE };
	xrWaitFrame(xr_session, nullptr, &frame_state);
	xrBeginFrame(xr_session, nullptr);

	openxr_poll_predicted(frame_state.predictedDisplayTime);
	app_update_predicted();

	XrCompositionLayerBaseHeader* layer = nullptr;
	XrCompositionLayerProjection             layer_proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	std::vector<XrCompositionLayerProjectionView> views;
	bool session_active = xr_session_state == XR_SESSION_STATE_VISIBLE || xr_session_state == XR_SESSION_STATE_FOCUSED;
	if (session_active && openxr_render_layer(frame_state.predictedDisplayTime, views, layer_proj)) {
		layer = (XrCompositionLayerBaseHeader*)&layer_proj;
	}

	XrFrameEndInfo end_info{ XR_TYPE_FRAME_END_INFO };
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = xr_blend;
	end_info.layerCount = layer == nullptr ? 0 : 1;
	end_info.layers = &layer;
	xrEndFrame(xr_session, &end_info);
}

bool openxr_render_layer(XrTime predictedTime, std::vector<XrCompositionLayerProjectionView>& views, XrCompositionLayerProjection& layer) {
	uint32_t         view_count = 0;
	XrViewState      view_state = { XR_TYPE_VIEW_STATE };
	XrViewLocateInfo locate_info = { XR_TYPE_VIEW_LOCATE_INFO };
	locate_info.viewConfigurationType = app_config_view;
	locate_info.displayTime = predictedTime;
	locate_info.space = xr_app_space;
	xrLocateViews(xr_session, &locate_info, &view_state, (uint32_t)xr_views.size(), &view_count, xr_views.data());
	views.resize(view_count);

	for (uint32_t i = 0; i < view_count; i++) {
		uint32_t                    img_id;
		XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		xrAcquireSwapchainImage(xr_swapchains[i].handle, &acquire_info, &img_id);

		XrSwapchainImageWaitInfo wait_info = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait_info.timeout = XR_INFINITE_DURATION;
		xrWaitSwapchainImage(xr_swapchains[i].handle, &wait_info);

		views[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
		views[i].pose = xr_views[i].pose;
		views[i].fov = xr_views[i].fov;
		views[i].subImage.swapchain = xr_swapchains[i].handle;
		views[i].subImage.imageRect.offset = { 0, 0 };
		views[i].subImage.imageRect.extent = { xr_swapchains[i].width, xr_swapchains[i].height };

		d3d_render_layer(views[i], xr_swapchains[i].surface_data[img_id]);

		XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xr_swapchains[i].handle, &release_info);
	}

	layer.space = xr_app_space;
	layer.viewCount = (uint32_t)views.size();
	layer.views = views.data();
	return true;
}
