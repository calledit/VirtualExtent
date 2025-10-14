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
