#pragma once

#include <windows.h>

#include <d3d9types.h>

// What a texture contains, judged from its texels.
enum class TextureContent {
    Unknown,   // not analysed, unsupported format or too small
    Color,     // anything that is not a normal map (albedo, masks, specular, ...)
    NormalMap, // tangent-space normal map: texels decode to unit vectors pointing outwards
};

const char* ToString(TextureContent content);

// Classifies one locked mip level. Supports DXT1-5 (block endpoint colours are sampled) and
// 32-bit A8R8G8B8 / X8R8G8B8. `pitch` is the row pitch from D3DLOCKED_RECT: bytes per row of
// pixels, or per row of 4x4 blocks for DXT.
TextureContent ClassifyTexture(D3DFORMAT format, UINT width, UINT height, const void* bits, INT pitch);
