#include "pch.h"
// Tell OpenXR what platform code we'll be using
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include "openxr.h"

#include <string>
#include <sstream>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <stdexcept>

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

	//printf("OpenXR extensions available:\n");
	for (size_t i = 0; i < xr_exts.size(); i++) {
		//printf("- %s\n", xr_exts[i].extensionName);
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
	XrResult result = xrCreateInstance(&createInfo, &xr_instance);
	if (XR_FAILED(result)) {
		OutputDebugStringA("Failed to create XrInstance\n");
		return false;
	}

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

void print_action_create_info_debug(const XrActionCreateInfo& aci) {
	std::stringstream ss;

	ss << "=== XrActionCreateInfo Debug ===\n";
	ss << "actionName: " << aci.actionName << "\n";
	ss << "localizedActionName: " << aci.localizedActionName << "\n";

	ss << "actionType: ";
	switch (aci.actionType) {
	case XR_ACTION_TYPE_BOOLEAN_INPUT:    ss << "BOOLEAN_INPUT"; break;
	case XR_ACTION_TYPE_FLOAT_INPUT:      ss << "FLOAT_INPUT"; break;
	case XR_ACTION_TYPE_VECTOR2F_INPUT:   ss << "VECTOR2F_INPUT"; break;
	case XR_ACTION_TYPE_POSE_INPUT:       ss << "POSE_INPUT"; break;
	case XR_ACTION_TYPE_VIBRATION_OUTPUT: ss << "VIBRATION_OUTPUT"; break;
	default:                              ss << "UNKNOWN"; break;
	}
	ss << "\n";

	ss << "countSubactionPaths: " << aci.countSubactionPaths << "\n";
	for (uint32_t i = 0; i < aci.countSubactionPaths; ++i) {
		char pathStr[256];
		uint32_t len = 0;
		xrPathToString(xr_instance, aci.subactionPaths[i], sizeof(pathStr), &len, pathStr);
		ss << "  subactionPath[" << i << "]: " << pathStr << "\n";
	}

	ss << "=== End of Action Info ===\n";

	std::string output = ss.str();
	OutputDebugStringA(output.c_str());
}

XrActionType determine_action_type_from_path(const std::string& path) {
	if (path.ends_with("/pose"))
		return XR_ACTION_TYPE_POSE_INPUT;
	if (path.ends_with("/click") || path.ends_with("/touch"))
		return XR_ACTION_TYPE_BOOLEAN_INPUT;
	if (path.ends_with("/value") || path.ends_with("/squeeze"))
		return XR_ACTION_TYPE_FLOAT_INPUT;
	if (path.ends_with("/thumbstick"))
		return XR_ACTION_TYPE_VECTOR2F_INPUT;

	throw std::runtime_error("Unknown action type for path: " + path);
}

bool openxr_generate_actions(ControllerProfile& profile) {
	// Create Action Set

	XrActionSetCreateInfo actionset_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(actionset_info.actionSetName, "gameplay");
	strcpy_s(actionset_info.localizedActionSetName, "Gameplay");

	if (XR_FAILED(xrCreateActionSet(xr_instance, &actionset_info, &xr_input.actionSet))) {
		OutputDebugStringA("Failed to create action set\n");
		return false;
	}

	// Create hand subpaths
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
	////_OATS] Unsupported path: /user/hand/left on interaction profile /interaction_profiles/oculus/touch_controller
	// Create action spaces for the hand poses
	for (int32_t i = 0; i < 2; i++) {
		XrActionSpaceCreateInfo action_space_info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		action_space_info.action = xr_input.poseAction;
		action_space_info.poseInActionSpace = xr_pose_identity;
		action_space_info.subactionPath = xr_input.handSubactionPath[i];
		xrCreateActionSpace(xr_session, &action_space_info, &xr_input.handSpace[i]);
	}

	std::vector<XrActionSuggestedBinding> bindings;

	XrPath pose_path[2];
	xrStringToPath(xr_instance, "/user/hand/left/input/grip/pose", &pose_path[0]);
	xrStringToPath(xr_instance, "/user/hand/right/input/grip/pose", &pose_path[1]);
	bindings.push_back({ xr_input.poseAction, pose_path[0] });
	bindings.push_back({ xr_input.poseAction, pose_path[1] });

	

	// Loop through JSON-defined mappings --------------------------------------------------
	for (size_t i = 0; i < profile.map.size(); ++i) {
		auto& m = profile.map[i];

		// Create path for this input
		if (XR_FAILED(xrStringToPath(xr_instance, m.name.c_str(), &m.xr_path))) {
			OutputDebugStringA(("Failed to convert path: " + m.name + "\n").c_str());
			continue;
		}

		// Determine correct action type based on suffix
		XrActionType actionType;
		try {
			actionType = determine_action_type_from_path(m.name);
		}
		catch (const std::exception& e) {
			OutputDebugStringA((std::string("Error: ") + e.what() + "\n").c_str());
			continue;
		}

		m.xr_actionType = actionType;

		// Build a safe action name (OpenXR requires valid identifier)
		std::string actionName = m.name;
		std::replace(actionName.begin(), actionName.end(), '/', '_');
		actionName += "_" + std::to_string(i); // append index for uniqueness

		XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
		aci.actionType = actionType;
		strncpy_s(aci.actionName, actionName.c_str(), XR_MAX_ACTION_NAME_SIZE - 1);
		strncpy_s(aci.localizedActionName, actionName.c_str(), XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);

		// Choose left or right hand subaction
		if (m.name.starts_with("/user/hand/left")) {
			aci.countSubactionPaths = 1;
			aci.subactionPaths = &xr_input.handSubactionPath[0];
		}
		else {
			aci.countSubactionPaths = 1;
			aci.subactionPaths = &xr_input.handSubactionPath[1];
		}

		// Create action
		if (XR_FAILED(xrCreateAction(xr_input.actionSet, &aci, &m.xr_action))) {
			print_action_create_info_debug(aci);
			OutputDebugStringA(("Failed to create action for: " + m.name + "\n").c_str());
			continue;
		}

		// Suggested binding entry for this mapping
		bindings.push_back({ m.xr_action, m.xr_path });

		// Create pose action spaces
		if (actionType == XR_ACTION_TYPE_POSE_INPUT) {
			XrActionSpaceCreateInfo asci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
			asci.action = m.xr_action;
			asci.poseInActionSpace = xr_pose_identity;

			if (m.name.starts_with("/user/hand/left"))
				asci.subactionPath = xr_input.handSubactionPath[0];
			else
				asci.subactionPath = xr_input.handSubactionPath[1];

			if (XR_SUCCEEDED(xrCreateActionSpace(xr_session, &asci, &m.xr_space))) {
				if (m.name.starts_with("/user/hand/left"))
					xr_input.handSpace[0] = m.xr_space;
				else
					xr_input.handSpace[1] = m.xr_space;
			}
			else {
				OutputDebugStringA(("Failed to create pose space for: " + m.name + "\n").c_str());
			}
		}
	}

	// Suggest the bindings ---------------------------------------------------------
	XrPath profilePath;
	if (XR_FAILED(xrStringToPath(xr_instance, profile.name.c_str(), &profilePath))) {
		OutputDebugStringA(("Invalid interaction profile: " + profile.name + "\n").c_str());
		return false;
	}

	XrInteractionProfileSuggestedBinding bindingInfo = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	bindingInfo.interactionProfile = profilePath;
	bindingInfo.countSuggestedBindings = (uint32_t)bindings.size();
	bindingInfo.suggestedBindings = bindings.data();

	if (XR_FAILED(xrSuggestInteractionProfileBindings(xr_instance, &bindingInfo))) {
		OutputDebugStringA("Failed to suggest interaction profile bindings\n");
		return false;
	}

	// Attach action set to the OpenXR session --------------------------------------
	XrSessionActionSetsAttachInfo attach_info = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach_info.countActionSets = 1;
	attach_info.actionSets = &xr_input.actionSet;

	if (XR_FAILED(xrAttachSessionActionSets(xr_session, &attach_info))) {
		OutputDebugStringA("Failed to attach action sets\n");
		return false;
	}

	return true;
}


void poll_controller_profile(ControllerProfile& profile) {
	if (xr_session_state != XR_SESSION_STATE_FOCUSED)
		return;

	XrActiveActionSet action_set = {};
	action_set.actionSet = xr_input.actionSet;
	action_set.subactionPath = XR_NULL_PATH;

	XrActionsSyncInfo sync_info = { XR_TYPE_ACTIONS_SYNC_INFO };
	sync_info.countActiveActionSets = 1;
	sync_info.activeActionSets = &action_set;
	xrSyncActions(xr_session, &sync_info);

	for (auto& m : profile.map) {
		XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
		gi.action = m.xr_action;
		gi.subactionPath = m.name.starts_with("/user/hand/left") ?
			xr_input.handSubactionPath[0] : xr_input.handSubactionPath[1];

		switch (m.xr_actionType) {
		case XR_ACTION_TYPE_BOOLEAN_INPUT: {
			XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
			if (XR_SUCCEEDED(xrGetActionStateBoolean(xr_session, &gi, &state)) && state.isActive) {
				
				bool cur_bol = state.currentState ? XR_TRUE : XR_FALSE;
				if (cur_bol != m.last_bool_state) {
					deal_with_bool_action(m, cur_bol);
					m.last_bool_state = cur_bol;
				}
				
			}
			break;
		}
		case XR_ACTION_TYPE_FLOAT_INPUT: {
			XrActionStateFloat state{ XR_TYPE_ACTION_STATE_FLOAT };
			if (XR_SUCCEEDED(xrGetActionStateFloat(xr_session, &gi, &state)) && state.isActive) {
				if (state.currentState != m.last_float_state) {
					deal_with_float_action(m, state.currentState, m.last_float_state);
					m.last_float_state = state.currentState;
				}
				
			}
			break;
		}
		case XR_ACTION_TYPE_VECTOR2F_INPUT: {
			XrActionStateVector2f state{ XR_TYPE_ACTION_STATE_VECTOR2F };
			if (XR_SUCCEEDED(xrGetActionStateVector2f(xr_session, &gi, &state)) && state.isActive) {

				if (m.is_x.has_value())
				{
					bool is_x = m.is_x.value();
					float value = is_x ? state.currentState.x : state.currentState.y;

					if ( value != m.last_float_state) {
						deal_with_float_action(m, value, m.last_float_state);
						m.last_float_state = value;
					}
				}
			}
			break;
		}
		case XR_ACTION_TYPE_POSE_INPUT: { //seams to not work as all poses need to be a part of the same xr_space or somthing like that
			OutputDebugStringA("pose:");
			if (m.xr_space != XR_NULL_HANDLE) {
				OutputDebugStringA("NOt null:");
				XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };

				//XrResult res = xrLocateSpace(xr_input.handSpace[hand], xr_app_space, select_state.lastChangeTime, &space_location);
				if (XR_SUCCEEDED(xrLocateSpace(m.xr_space, xr_app_space, 0 /* use predicted time later */, &loc)) &&
					(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
					(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {

					OutputDebugStringA("suceeed:");

					bool valid_pose = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
						(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);


					m.xr_posef = loc.pose;

					if (m.name.starts_with("/user/hand/left")) {
						xr_input.renderHand[0] = valid_pose ? XR_TRUE : XR_FALSE;
					}
					else {
						xr_input.renderHand[1] = valid_pose ? XR_TRUE : XR_FALSE;
					}

					std::stringstream ss;
					ss << "[pose] " << m.name << ": pos("
						<< loc.pose.position.x << ", "
						<< loc.pose.position.y << ", "
						<< loc.pose.position.z << ") rot("
						<< loc.pose.orientation.x << ", "
						<< loc.pose.orientation.y << ", "
						<< loc.pose.orientation.z << ", "
						<< loc.pose.orientation.w << ")\n";
					OutputDebugStringA(ss.str().c_str());
				}
			}
			break;
		}
		default:
			OutputDebugStringA(("Unknown action type for: " + m.name + "\n").c_str());
			break;
		}
	}
}




void openxr_poll_predicted(XrTime predicted_time) {
	if (xr_session_state != XR_SESSION_STATE_FOCUSED)
		return;

	for (size_t i = 0; i < 2; i++) {
		XrSpaceLocation spaceRelation = { XR_TYPE_SPACE_LOCATION };
		XrResult        res = xrLocateSpace(xr_input.handSpace[i], xr_app_space, predicted_time, &spaceRelation);
		if (XR_UNQUALIFIED_SUCCESS(res) &&
			(spaceRelation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
			(spaceRelation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
			xr_input.handPose[i] = spaceRelation.pose;
			xr_input.renderHand[i] = XR_TRUE;
		}
		else {
			xr_input.renderHand[i] = XR_FALSE;
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
