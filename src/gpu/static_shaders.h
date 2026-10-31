#pragma once
#include <vector>

#include <regpu/regpu.h>

// Every shader the title ships, for compiling them all ahead of time on the first boot (Host::staticShaders).
std::vector<regpu::ScannedShader> StaticShaders();
// Whether the list is saved for this executable (Host::staticShadersSaved: no search needed).
bool StaticShadersSaved();
