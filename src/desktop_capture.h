#pragma once
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>

// Simple D3D11 desktop duplicator -> shader-readable SRV per frame.
class DesktopCapture {
public:
	// device must be your rendering device; adapter must be the SAME GPU as that device.
	bool Init(ID3D11Device* device, IDXGIAdapter1* adapter, int outputIndex = 0);
	void Shutdown();

	// Acquire next desktop frame into an internal shader-readable texture.
	// Returns false on timeout (no new frame) or failure.
	// If true, outSrv is AddRef'ed; caller must Release when done binding.
	bool Acquire(ID3D11DeviceContext* ctx,
		ID3D11ShaderResourceView** outSrv,
		uint32_t* outW,
		uint32_t* outH,
		uint32_t timeoutMs = 0);

private:
	Microsoft::WRL::ComPtr<IDXGIOutputDuplication>   dup_;
	Microsoft::WRL::ComPtr<ID3D11Texture2D>          texSR_;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
	uint32_t     w_ = 0, h_ = 0;
	DXGI_FORMAT  fmt_ = DXGI_FORMAT_B8G8R8A8_UNORM;

	bool recreateTexture(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT fmt);
};
