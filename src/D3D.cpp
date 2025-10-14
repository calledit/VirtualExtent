// Länkbibliotek kan ligga kvar i main.cpp, men går bra här också om du vill
//#pragma comment(lib,"D3D11.lib")
//#pragma comment(lib,"D3dcompiler.lib")
//#pragma comment(lib,"Dxgi.lib")

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include "d3d.h"
#include <dxgi.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstring>

using namespace DirectX;

// Globala D3D-objekt
ID3D11Device* d3d_device = nullptr;
ID3D11DeviceContext* d3d_context = nullptr;
int64_t              d3d_swapchain_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

bool d3d_init(LUID& adapter_luid) {
	IDXGIAdapter1* adapter = d3d_get_adapter(adapter_luid);
	D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };

	if (adapter == nullptr)
		return false;
	if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, 0, 0,
		featureLevels, _countof(featureLevels), D3D11_SDK_VERSION,
		&d3d_device, nullptr, &d3d_context)))
		return false;

	adapter->Release();
	return true;
}

IDXGIAdapter1* d3d_get_adapter(LUID& adapter_luid) {
	IDXGIAdapter1* final_adapter = nullptr;
	IDXGIAdapter1* curr_adapter = nullptr;
	IDXGIFactory1* dxgi_factory;
	DXGI_ADAPTER_DESC1 adapter_desc;

	CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)(&dxgi_factory));

	int curr = 0;
	while (dxgi_factory->EnumAdapters1(curr++, &curr_adapter) == S_OK) {
		curr_adapter->GetDesc1(&adapter_desc);

		if (memcmp(&adapter_desc.AdapterLuid, &adapter_luid, sizeof(&adapter_luid)) == 0) {
			final_adapter = curr_adapter;
			break;
		}
		curr_adapter->Release();
		curr_adapter = nullptr;
	}
	dxgi_factory->Release();
	return final_adapter;
}

void d3d_shutdown() {
	if (d3d_context) { d3d_context->Release(); d3d_context = nullptr; }
	if (d3d_device) { d3d_device->Release();  d3d_device = nullptr; }
}

swapchain_surfdata_t d3d_make_surface_data(XrBaseInStructure& swapchain_img) {
	swapchain_surfdata_t result = {};

	XrSwapchainImageD3D11KHR& d3d_swapchain_img = (XrSwapchainImageD3D11KHR&)swapchain_img;
	D3D11_TEXTURE2D_DESC      color_desc;
	d3d_swapchain_img.texture->GetDesc(&color_desc);

	D3D11_RENDER_TARGET_VIEW_DESC target_desc = {};
	target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	target_desc.Format = (DXGI_FORMAT)d3d_swapchain_fmt;
	d3d_device->CreateRenderTargetView(d3d_swapchain_img.texture, &target_desc, &result.target_view);

	ID3D11Texture2D* depth_texture;
	D3D11_TEXTURE2D_DESC depth_desc = {};
	depth_desc.SampleDesc.Count = 1;
	depth_desc.MipLevels = 1;
	depth_desc.Width = color_desc.Width;
	depth_desc.Height = color_desc.Height;
	depth_desc.ArraySize = color_desc.ArraySize;
	depth_desc.Format = DXGI_FORMAT_R32_TYPELESS;
	depth_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
	d3d_device->CreateTexture2D(&depth_desc, nullptr, &depth_texture);

	D3D11_DEPTH_STENCIL_VIEW_DESC stencil_desc = {};
	stencil_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	stencil_desc.Format = DXGI_FORMAT_D32_FLOAT;
	d3d_device->CreateDepthStencilView(depth_texture, &stencil_desc, &result.depth_view);

	depth_texture->Release();

	return result;
}

void d3d_render_layer(XrCompositionLayerProjectionView& view, swapchain_surfdata_t& surface) {
	XrRect2Di& rect = view.subImage.imageRect;
	D3D11_VIEWPORT viewport = CD3D11_VIEWPORT((float)rect.offset.x, (float)rect.offset.y, (float)rect.extent.width, (float)rect.extent.height);
	d3d_context->RSSetViewports(1, &viewport);

	float clear[] = { 0, 0, 0, 1 };
	d3d_context->ClearRenderTargetView(surface.target_view, clear);
	d3d_context->ClearDepthStencilView(surface.depth_view, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
	d3d_context->OMSetRenderTargets(1, &surface.target_view, surface.depth_view);

	// Appens draw-funktion ligger kvar i main.cpp, och kallas från openxr.cpp via d3d_render_layer->app_draw där.
	extern void app_draw(XrCompositionLayerProjectionView & layerView);
	app_draw(view);
}

void d3d_swapchain_destroy(swapchain_t& swapchain) {
	for (uint32_t i = 0; i < swapchain.surface_data.size(); i++) {
		swapchain.surface_data[i].depth_view->Release();
		swapchain.surface_data[i].target_view->Release();
	}
}

XMMATRIX d3d_xr_projection(XrFovf fov, float clip_near, float clip_far) {
	const float left = clip_near * tanf(fov.angleLeft);
	const float right = clip_near * tanf(fov.angleRight);
	const float down = clip_near * tanf(fov.angleDown);
	const float up = clip_near * tanf(fov.angleUp);
	return XMMatrixPerspectiveOffCenterRH(left, right, down, up, clip_near, clip_far);
}

ID3DBlob* d3d_compile_shader(const char* hlsl, const char* entrypoint, const char* target) {
	DWORD flags = D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob* compiled = nullptr, * errors = nullptr;
	if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entrypoint, target, flags, 0, &compiled, &errors)))
		printf("Error: D3DCompile failed %s", errors ? (char*)errors->GetBufferPointer() : "(no message)");
	if (errors) errors->Release();

	return compiled;
}
