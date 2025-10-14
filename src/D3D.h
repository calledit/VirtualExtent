#pragma once

// Måste vara definierade innan OpenXR-headrar inkluderas
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D11
#endif

#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include "openxr.h" // för swapchain_surfdata_t och swapchain_t

// Globala D3D-objekt som appen använder
extern ID3D11Device* d3d_device;
extern ID3D11DeviceContext* d3d_context;
extern int64_t              d3d_swapchain_fmt;

// Init/teardown
bool           d3d_init(LUID& adapter_luid);
void           d3d_shutdown();
IDXGIAdapter1* d3d_get_adapter(LUID& adapter_luid);

// Ytor/swapchain-hjälp
swapchain_surfdata_t d3d_make_surface_data(XrBaseInStructure& swapchainImage);
void                 d3d_swapchain_destroy(swapchain_t& swapchain);

// Rendering och proj-matris
void                 d3d_render_layer(XrCompositionLayerProjectionView& layerView, swapchain_surfdata_t& surface);
DirectX::XMMATRIX    d3d_xr_projection(XrFovf fov, float clip_near, float clip_far);

// Shaderkompilering
ID3DBlob* d3d_compile_shader(const char* hlsl, const char* entrypoint, const char* target);
