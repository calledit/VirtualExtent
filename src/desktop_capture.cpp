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

	// Map incoming UNORM format to a TYPELESS resource + SRGB SRV
	auto toTypeless = [](DXGI_FORMAT f) -> DXGI_FORMAT {
		switch (f) {
		case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
		case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
			// Already SRGB/TYPELESS or unsupported: leave as-is
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			return f;
		default:
			return f;
		}
		};
	auto toSRGBView = [](DXGI_FORMAT f) -> DXGI_FORMAT {
		switch (f) {
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:   return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:   return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		default:                               return f; // fallback: no srgb view
		}
		};

	DXGI_FORMAT resFmt = toTypeless(fmt);
	DXGI_FORMAT srvFmt = toSRGBView(fmt);

	// 1) Create the shader-readable texture (TYPELESS when possible)
	D3D11_TEXTURE2D_DESC d{};
	d.Width = w;
	d.Height = h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = resFmt;             // TYPELESS preferred
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	d.CPUAccessFlags = 0;
	d.MiscFlags = 0;

	if (FAILED(device->CreateTexture2D(&d, nullptr, &texSR_)))
		return false;

	// 2) Create SRV — prefer SRGB view so sampling returns linear
	D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
	sv.Format = srvFmt;
	sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sv.Texture2D.MostDetailedMip = 0;
	sv.Texture2D.MipLevels = 1;

	HRESULT hr = device->CreateShaderResourceView(texSR_.Get(), &sv, &srv_);
	if (FAILED(hr)) {
		// Fallback: try standard UNORM view (no auto-linearization)
		sv.Format = fmt;
		hr = device->CreateShaderResourceView(texSR_.Get(), &sv, &srv_);
		if (FAILED(hr))
			return false;
		// Note: if you hit this path, keep the HLSL gamma fix (pow 2.2) in your plane shader.
	}

	w_ = w;
	h_ = h;
	fmt_ = fmt;
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
