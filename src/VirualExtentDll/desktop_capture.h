#pragma once
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>

class DesktopCapture {
public:
    // Initialize duplication for adapter->EnumOutputs(outputIndex)
    bool Init(ID3D11Device* device, IDXGIAdapter1* adapter, int outputIndex);
    void Shutdown();

    // Acquire next frame.
    // Returns true only when a NEW frame was copied this call.
    // On false, you may still get outSrv set to the last good frame (so you can keep rendering),
    // but you should continue calling Acquire each frame so we can recover after mode switches.
    bool Acquire(ID3D11DeviceContext* ctx,
        ID3D11ShaderResourceView** outSrv,
        uint32_t* outW,
        uint32_t* outH,
        uint32_t timeoutMs);

    // Optional: call when you receive WM_DISPLAYCHANGE
    void NotifyDisplayModeChange();

private:
    bool recreateTexture(ID3D11Device* device, uint32_t w, uint32_t h, DXGI_FORMAT fmt);
    bool tryCreateDuplication(); // attempt once; no loops

    // Duplication + shader-readable copy of latest frame
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        texSR_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;

    // Cached so we can transparently recover after DXGI_ERROR_ACCESS_LOST / NOT_CURRENTLY_AVAILABLE
    Microsoft::WRL::ComPtr<ID3D11Device>   device_;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>  adapter_;
    int                                    outputIndex_ = 0;

    // Recovery state
    bool        needRecreateDup_ = false;   // set when we detect loss or you call NotifyDisplayModeChange
    uint64_t    nextRetryMs_ = 0;       // GetTickCount64() time for next retry
    uint32_t    retryBackoffMs_ = 250;     // adaptive backoff (caps at 2s)

    uint32_t    w_ = 0, h_ = 0;
    DXGI_FORMAT fmt_ = DXGI_FORMAT_B8G8R8A8_UNORM;
};