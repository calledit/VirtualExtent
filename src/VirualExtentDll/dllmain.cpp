// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"

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
#include "controller_config.h"

static ControllerConfig controllerConfig;
static ControllerProfile* selectedProfile = nullptr;




extern "C" {
    __declspec(dllexport) int VE_Start();      // start OpenXR loop
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

__declspec(dllexport) int VE_Start() {
	if (!openxr_init("VirtualExtent", d3d_swapchain_fmt)) {
		d3d_shutdown();
		MessageBox(nullptr, L"OpenXR initialization failed\n", L"Error", 1);
		return 1;
	}

	// Load controller map
	if (load_controller_config("C:\\Users\\calle\\projects\\VirtualExtent\\controller_map.json", controllerConfig)) {
		if (!controllerConfig.controller_maps.empty()) {
			selectedProfile = &controllerConfig.controller_maps.back();
			printf("Selected controller profile: %s\n", selectedProfile->name.c_str());
		}
	}
	else {
		printf("Failed to load controller config. Using built-in fallback.\n");
	}

	OutputDebugStringA("Controller config loaded: exe\n\n\n\n\n\n\n\n\n\n");
	print_controller_map(*selectedProfile);
	//OutputDebugStringA(controllerConfig.exe_name_hash.c_str());
	openxr_generate_actions(*selectedProfile);

	// Init modules
	//Cubes_Init();
	DesktopPlane_Init(/*widthMeters*/3.0f, /*distanceMeters*/1.5f, /*outputIndex*/0);
	Controllers_Init();

	bool quit = false;
	while (!quit) {
		openxr_poll_events(quit);

		if (xr_running) {
			poll_controller_profile(*selectedProfile);
			openxr_render_frame();

			if (xr_session_state != XR_SESSION_STATE_VISIBLE &&
				xr_session_state != XR_SESSION_STATE_FOCUSED) {
				this_thread::sleep_for(chrono::milliseconds(250));
			}
		}
	}

	// Shutdown modules
	DesktopPlane_Shutdown();
	//Cubes_Shutdown();
	Controllers_Shutdown();

	openxr_shutdown();
	d3d_shutdown();
	return 0;
}


void app_update_predicted() {
    // Called between xrBeginFrame and app draw; keep cube hands predicted
    Cubes_UpdatePredicted();
}

void app_draw(XrCompositionLayerProjectionView& view) {
    // 1) Draw the fixed desktop plane
    DesktopPlane_Draw(view);

    Controllers_Draw(view);

    // 2) Draw the cubes
    //Cubes_Draw(view);
}
