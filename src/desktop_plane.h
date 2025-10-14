#pragma once
#include <openxr/openxr.h>

// Initializes the capture + plane resources.
// widthMeters: plane width (X) in meters; height is derived from desktop aspect.
// distanceMeters: distance from origin along the user's initial gaze at first draw.
// outputIndex: which monitor to capture (0 = primary).
bool DesktopPlane_Init(float widthMeters = 3.0f, float distanceMeters = 1.5f, int outputIndex = 0);

// Draws the plane for the current eye view. Keeps the plane fixed in app space,
// placed once from the initial head orientation at first draw.
void DesktopPlane_Draw(const XrCompositionLayerProjectionView& view);

// Releases all resources.
void DesktopPlane_Shutdown();

/** Raycast from world-space origin+dir against the desktop plane.
    Returns true if the ray hits the plane *inside* the quad.
    outWorldHit : world-space hit position (meters)
    outUV       : [0..1] UV using the *same mirroring* as the plane shader
*/
bool DesktopPlane_Raycast(const DirectX::XMVECTOR& rayOrigin,
    const DirectX::XMVECTOR& rayDir,
    DirectX::XMFLOAT3* outWorldHit,
    DirectX::XMFLOAT2* outUV);

// Map plane UV (already mirrored like your shader) to desktop virtual-screen coords.
// Returns false if plane not placed or capture not ready.
bool DesktopPlane_UVToScreen(const DirectX::XMFLOAT2& uv, POINT* outScreenPt);

// Convenience: warp the system cursor; returns false if mapping failed.
bool DesktopPlane_WarpCursor(const DirectX::XMFLOAT2& uv);
