#pragma once

// Make sure these two are defined BEFORE including this header in any TU.
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif

#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>

// Shared types (used by both D3D code and OpenXR code)
struct swapchain_surfdata_t {
	ID3D11DepthStencilView* depth_view;
	ID3D11RenderTargetView* target_view;
};

struct swapchain_t {
	XrSwapchain handle;
	int32_t     width;
	int32_t     height;
	std::vector<XrSwapchainImageD3D11KHR> surface_images;
	std::vector<swapchain_surfdata_t>     surface_data;
};

struct input_state_t {
	XrActionSet actionSet;
	XrAction    poseAction;
	XrAction    selectAction;
	XrPath   handSubactionPath[2];
	XrSpace  handSpace[2];
	XrPosef  handPose[2];
	XrBool32 renderHand[2];
	XrBool32 handSelect[2];
};

// OpenXR globals that the app (or D3D code) needs to see
extern const XrPosef  xr_pose_identity;
extern input_state_t  xr_input;
extern XrSessionState xr_session_state;
extern bool           xr_running;

// OpenXR API
bool openxr_init(const char* app_name, int64_t swapchain_format);
void openxr_make_actions();
void openxr_shutdown();
void openxr_poll_events(bool& exit);
void openxr_poll_actions();
void openxr_poll_predicted(XrTime predicted_time);
void openxr_render_frame();
bool openxr_render_layer(XrTime predictedTime, std::vector<XrCompositionLayerProjectionView>& projectionViews, XrCompositionLayerProjection& layer);
