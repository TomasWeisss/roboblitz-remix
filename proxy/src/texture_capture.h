#pragma once

#include <d3d9.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Keeps a copy of the top mip level of every texture the game creates, taken when it unlocks the
// level after writing it, or copies it in with UpdateTexture. Reading textures back through the
// Remix bridge is not reliable, so this is the only way to get at their contents. Costs as much
// memory as the game's textures.

struct CapturedTexture {
    D3DFORMAT format = D3DFMT_UNKNOWN;
    UINT width = 0;
    UINT height = 0;
    std::vector<uint8_t> level0; // tightly packed (GetLevelLayout)
};

// Hooks the device's CreateTexture and UpdateTexture and, through the first texture, the
// LockRect/UnlockRect of textures and surfaces
void InstallTextureCapture(IDirect3DDevice9* device);

// Diagnostics for a texture that was not captured: its locks that were not, e.g.
// "L1 f0 hr0 p2048" (level, flags, " rect" if partial, result, pitch; level 99: through a surface
// other than the top level); "never locked"
std::string DescribeTextureLocks(IDirect3DBaseTexture9* texture);

// The texture's top level as last written by the game; null if never written (render targets,
// cube maps, textures filled by other means)
std::shared_ptr<const CapturedTexture> FindCapturedTexture(IDirect3DBaseTexture9* texture);
