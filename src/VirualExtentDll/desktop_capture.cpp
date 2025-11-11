#include "pch.h"
#define WIN32_LEAN_AND_MEAN
#include "desktop_capture.h"

#include <cstdio>
#include <Windows.h> // GetTickCount64

using Microsoft::WRL::ComPtr;

bool DesktopCapture::Init(ID3D11Device* device, IDXGIAdapter1* adapter, int outputIndex) {
    if (!device || !adapter) return false;

    device_ = device;       // cache for recovery
    adapter_ = adapter;     // cache for recovery
    outputIndex_ = outputIndex;

    if (!tryCreateDuplication())
        return false;

    // Reset cached *dimensions* so we recreate texture on first Acquire.
    w_ = h_ = 0;
    fmt_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    texSR_.Reset();
    srv_.Reset();

    needRecreateDup_ = false;
    retryBackoffMs_ = 250;
    nextRetryMs_ = 0;
    return true;
}

void DesktopCapture::Shutdown() {
    srv_.Reset();
    texSR_.Reset();
    dup_.Reset();
    device_.Reset();
    adapter_.Reset();
    w_ = h_ = 0;
    fmt_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    outputIndex_ = 0;
    needRecreateDup_ = false;
    retryBackoffMs_ = 250;
    nextRetryMs_ = 0;
}

void DesktopCapture::NotifyDisplayModeChange() {
    // Don’t nuke the SRV; keep last good frame visible.
    needRecreateDup_ = true;
    // retry soon
    nextRetryMs_ = GetTickCount64();
    retryBackoffMs_ = 250;
}

bool DesktopCapture::recreateTexture(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    // Create NEW resources and then swap them in so anyone holding the old SRV still sees something.
    ComPtr<ID3D11Texture2D>          newTex;
    ComPtr<ID3D11ShaderResourceView> newSrv;

    auto toTypeless = [](DXGI_FORMAT f) -> DXGI_FORMAT {
        switch (f) {
        case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return f;
        default: return f;
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
        default:                               return f;
        }
        };

    DXGI_FORMAT resFmt = toTypeless(fmt);
    DXGI_FORMAT srvFmt = toSRGBView(fmt);

    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h;
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = resFmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0; d.MiscFlags = 0;

    if (FAILED(device->CreateTexture2D(&d, nullptr, &newTex)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = srvFmt;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MostDetailedMip = 0;
    sv.Texture2D.MipLevels = 1;

    HRESULT hr = device->CreateShaderResourceView(newTex.Get(), &sv, &newSrv);
    if (FAILED(hr)) {
        sv.Format = fmt;
        if (FAILED(device->CreateShaderResourceView(newTex.Get(), &sv, &newSrv)))
            return false;
    }

    texSR_ = newTex;
    srv_ = newSrv;

    w_ = w; h_ = h; fmt_ = fmt;
    return true;
}

bool DesktopCapture::tryCreateDuplication() {
    if (!adapter_ || !device_)
        return false;

    ComPtr<IDXGIOutput> out;
    if (adapter_->EnumOutputs(outputIndex_, &out) != S_OK)
        return false;

    ComPtr<IDXGIOutput1> out1;
    if (FAILED(out.As(&out1)))
        return false;

    ComPtr<IDXGIOutputDuplication> dup;
    HRESULT hr = out1->DuplicateOutput(device_.Get(), &dup);
    if (FAILED(hr)) {
        // Most likely DXGI_ERROR_NOT_CURRENTLY_AVAILABLE while the game flips modes or holds exclusive.
        // Leave dup_ unchanged; caller will schedule a retry.
        return false;
    }

    dup_ = dup;

    // Force SRV recreation on next successful frame (resolution/format may have changed)
    w_ = h_ = 0;
    // Keep last good SRV visible
    needRecreateDup_ = false;
    retryBackoffMs_ = 250;
    nextRetryMs_ = 0;
    return true;
}

bool DesktopCapture::Acquire(ID3D11DeviceContext* ctx,
    ID3D11ShaderResourceView** outSrv,
    uint32_t* outW,
    uint32_t* outH,
    uint32_t timeoutMs) {
    // Locals declared before any gotos
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource>   res;
    HRESULT                 hr = S_OK;

    // If we know we must recreate, try at controlled intervals
    if (needRecreateDup_) {
        const uint64_t now = GetTickCount64();
        if (now >= nextRetryMs_) {
            if (!tryCreateDuplication()) {
                // schedule next retry with backoff (cap at 2s)
                retryBackoffMs_ = (retryBackoffMs_ < 2000) ? (retryBackoffMs_ * 2) : 2000;
                nextRetryMs_ = now + retryBackoffMs_;
            }
        }
        // Even if not yet recreated, keep returning last SRV below.
    }

    if (!dup_) {
        // Not ready yet; return last known frame if any
        goto return_last_no_new_frame;
    }

    hr = dup_->AcquireNextFrame(timeoutMs, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // No new frame this interval (could be occluded or game still switching)
        goto return_last_no_new_frame;
    }
	
    if (info.AccumulatedFrames > 1) {
        // Keep grabbing frames with zero timeout until AccumulatedFrames <= 1.
        // IMPORTANT: Release the current frame before acquiring the next.
        while (info.AccumulatedFrames > 1) {
            // Release the older frame we currently hold
            dup_->ReleaseFrame();

            info = {};
            res.Reset();
            HRESULT hr2 = dup_->AcquireNextFrame(0, &info, &res);
            if (hr2 == DXGI_ERROR_WAIT_TIMEOUT) {
                // No newer frame immediately available; stop here.
                // We do NOT have a frame held in this case, so go return last-known SRV.
                goto return_last_no_new_frame;
            }
            if (FAILED(hr2)) {
                // Something went wrong while trying to fast-forward; bail out cleanly.
                goto return_last_no_new_frame;
            }

            // Loop again if we're still behind. When AccumulatedFrames <= 1, we stop with a valid frame held.
        }
    }

    if (FAILED(hr)) {
        // Typical during mode switch or exclusive transition
        if (hr == DXGI_ERROR_ACCESS_LOST ||
            hr == DXGI_ERROR_DEVICE_REMOVED ||
            hr == DXGI_ERROR_INVALID_CALL) {
            // Mark for recreation and clear current dup
            dup_.Reset();
            needRecreateDup_ = true;
            nextRetryMs_ = GetTickCount64(); // retry soon
            goto return_last_no_new_frame;
        }
        else {
            // Unknown failure; also mark for recreate
            dup_.Reset();
            needRecreateDup_ = true;
            nextRetryMs_ = GetTickCount64();
            goto return_last_no_new_frame;
        }
    }

    // We have a new frame
    {
        ComPtr<ID3D11Texture2D> frameTex;
        if (FAILED(res.As(&frameTex))) {
            dup_->ReleaseFrame();
            goto return_last_no_new_frame;
        }

        D3D11_TEXTURE2D_DESC desc{};
        frameTex->GetDesc(&desc);

        // Recreate our shader-readable copy if size/format changed
        if (w_ != desc.Width || h_ != desc.Height || fmt_ != desc.Format || !texSR_) {
            ComPtr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            if (!recreateTexture(dev.Get(), desc.Width, desc.Height, desc.Format)) {
                dup_->ReleaseFrame();
                goto return_last_no_new_frame;
            }
        }

        // Copy the new frame into our SRV-backed texture
        ctx->CopyResource(texSR_.Get(), frameTex.Get());

        if (outSrv && srv_) { *outSrv = srv_.Get(); (*outSrv)->AddRef(); }
        if (outW) *outW = w_;
        if (outH) *outH = h_;

        dup_->ReleaseFrame();
        return true; // NEW frame delivered
    }

return_last_no_new_frame:
    // Provide last known SRV so caller can keep rendering if desired,
    // but signal "no new frame" by returning false.
    if (outSrv && srv_) { *outSrv = srv_.Get(); (*outSrv)->AddRef(); }
    if (outW) *outW = w_;
    if (outH) *outH = h_;
    return false;
}
