#pragma once
#include <openxr/openxr.h>

// Create shaders, buffers, states for controller rendering
bool Controllers_Init();

// Optional per-frame logic (not strictly required since you already
// call openxr_poll_predicted); provided to match your structure.
void Controllers_UpdatePredicted();

// Draws both hands (if active) for the current eye view
void Controllers_Draw(const XrCompositionLayerProjectionView& view);

// Release all resources
void Controllers_Shutdown();

void Controllers_HandleClicks();