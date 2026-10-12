#pragma once

#include "material_map.h"

// Hooks the game's D3DXCompileShader import. For every pixel shader compiled from a material, it
// records the material in the material map: its material functions (normal, diffuse, specular, ...),
// blend mode, lighting model, and which of them is the albedo: GetMaterialDiffuseColor, or
// GetMaterialEmissive for unlit materials (their diffuse colour is unused). If that samples no
// texture, the opacity (translucent) or opacity mask (masked) expression is used if it does.
void InstallCompilerHook();

// Loaded from roboblitz_remix_materials.txt next to the proxy
const MaterialMap& GetMaterialMap();
