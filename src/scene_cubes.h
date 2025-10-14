#pragma once
#include <openxr/openxr.h>

bool  Cubes_Init();
void  Cubes_Update();                    // reacts to hand select
void  Cubes_UpdatePredicted();           // keeps hand cubes at predicted pose
void  Cubes_Draw(const XrCompositionLayerProjectionView& view);
void  Cubes_Shutdown();
