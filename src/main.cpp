#pragma comment(lib,"D3D11.lib")
#pragma comment(lib,"D3dcompiler.lib")
#pragma comment(lib,"Dxgi.lib")

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <d3d11.h>
#include <directxmath.h>
#include <d3dcompiler.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>

#include <thread>
#include <vector>
#include <algorithm>

using namespace std;
using namespace DirectX;

#include "openxr.h"         // OpenXR API/globals
#include "d3d.h"            // D3D globals/functions
#include "desktop_plane.h"  // Desktop screen module
#include "scene_cubes.h"    // <-- NEW: cubes module
#include "controllers.h"

///////////////////////////////////////////
// Main                                  //
///////////////////////////////////////////

int __stdcall wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
	if (!openxr_init("VirtualExtent", d3d_swapchain_fmt)) {
		d3d_shutdown();
		MessageBox(nullptr, "OpenXR initialization failed\n", "Error", 1);
		return 1;
	}

	openxr_make_actions();

	// Init modules
	Cubes_Init();
	DesktopPlane_Init(/*widthMeters*/3.0f, /*distanceMeters*/1.5f, /*outputIndex*/0);
	Controllers_Init();

	bool quit = false;
	while (!quit) {
		openxr_poll_events(quit);

		if (xr_running) {
			openxr_poll_actions();
			Cubes_Update();               // hand select -> spawn cube
			openxr_render_frame();

			if (xr_session_state != XR_SESSION_STATE_VISIBLE &&
				xr_session_state != XR_SESSION_STATE_FOCUSED) {
				this_thread::sleep_for(chrono::milliseconds(250));
			}
		}
	}

	// Shutdown modules
	DesktopPlane_Shutdown();
	Cubes_Shutdown();
	Controllers_Shutdown();

	openxr_shutdown();
	d3d_shutdown();
	return 0;
}

///////////////////////////////////////////
// App hooks called by d3d/openxr layer  //
///////////////////////////////////////////

void app_update_predicted() {
	// Called between xrBeginFrame and app draw; keep cube hands predicted
	Cubes_UpdatePredicted();
	Controllers_UpdatePredicted();
}

void app_draw(XrCompositionLayerProjectionView& view) {
	// 1) Draw the fixed desktop plane
	DesktopPlane_Draw(view);

	Controllers_Draw(view);

	// 2) Draw the cubes
	Cubes_Draw(view);
}
