#pragma once
#include <openxr/openxr.h>

// Create shaders, buffers, states for controller rendering
bool Controllers_Init();


// Draws both hands (if active) for the current eye view
void Controllers_Draw(const XrCompositionLayerProjectionView& view);

// Release all resources
void Controllers_Shutdown();

void Controllers_HandleClicks();