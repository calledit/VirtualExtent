#define WIN32_LEAN_AND_MEAN
#include "desktop_capture.h"

#include <cstdio>

using Microsoft::WRL::ComPtr;

bool DesktopCapture::Init(ID3D11Device* device, IDXGIAdapter1* adapter, int outputIndex) {
	if (!device || !adapter) return false;

	// Pick the output (monitor)
	ComPtr<IDXGIOutput> out;
	if (adapter->EnumOutputs(outputIndex, &out) != S_OK)
		return false;

	ComPtr<IDXGIOutput1> out1;
	if (FAILED(out.As(&out1)))
		return false;

	// Create duplication
	ComPtr<IDXGIOutputDuplication> dup;
	HRESULT hr = out1->DuplicateOutput(device, &dup);
	if (FAILED(hr)) {
		// Common failure: DXGI_ERROR_NOT_CURRENTLY_AVAILABLE if too many duplications exist
		// or the GPU doesn't support DDA (very rare on Win10/11).
		return false;
	}

	dup_ = dup;
	// Reset cached resources; they’ll be created on first Acquire
	w_ = h_ = 0;
	fmt_ = DXGI_FORMAT_B8G8R8A8_UNORM;
	srv_.Reset();
	texSR_.Reset();
	return true;
}

void DesktopCapture::Shutdown() {
	srv_.Reset();
	texSR_.Reset();
	dup_.Reset();
	w_ = h_ = 0;
}

bool DesktopCapture::recreateTexture(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
	texSR_.Reset();
	srv_.Reset();

	// Create a shader-readable texture to copy frame data into
	D3D11_TEXTURE2D_DESC d{};
	d.Width = w;
	d.Height = h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = fmt;                 // DDA is typically BGRA8 UNORM
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	if (FAILED(device->CreateTexture2D(&d, nullptr, &texSR_)))
		return false;

	D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
	sv.Format = fmt;
	sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sv.Texture2D.MostDetailedMip = 0;
	sv.Texture2D.MipLevels = 1;
	if (FAILED(device->CreateShaderResourceView(texSR_.Get(), &sv, &srv_)))
		return false;

	w_ = w; h_ = h; fmt_ = fmt;
	return true;
}

bool DesktopCapture::Acquire(ID3D11DeviceContext* ctx,
	ID3D11ShaderResourceView** outSrv,
	uint32_t* outW,
	uint32_t* outH,
	uint32_t timeoutMs) {
	if (!dup_) return false;

	DXGI_OUTDUPL_FRAME_INFO info{};
	ComPtr<IDXGIResource> res;

	HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &res);
	if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
		// No new frame; caller should reuse last SRV if they want persistence
		return false;
	}
	if (FAILED(hr)) {
		// If we lose the duplication (monitor change, sleep, etc.), the app can re-Init().
		return false;
	}

	bool ok = true;

	do {
		ComPtr<ID3D11Texture2D> frameTex;
		if (FAILED(res.As(&frameTex))) { ok = false; break; }

		D3D11_TEXTURE2D_DESC desc{};
		frameTex->GetDesc(&desc);

		// Recreate our shader-readable copy if size/format changed
		if (w_ != desc.Width || h_ != desc.Height || fmt_ != desc.Format) {
			ComPtr<ID3D11Device> dev;
			ctx->GetDevice(&dev);
			if (!recreateTexture(dev.Get(), desc.Width, desc.Height, desc.Format)) { ok = false; break; }
		}

		// GPU copy the new frame into our SRV-backed texture
		ctx->CopyResource(texSR_.Get(), frameTex.Get());

		if (outSrv) { *outSrv = srv_.Get(); (*outSrv)->AddRef(); }
		if (outW) *outW = w_;
		if (outH) *outH = h_;

	} while (false);

	// Always release the frame
	dup_->ReleaseFrame();
	return ok;
}
