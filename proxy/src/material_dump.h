#pragma once

#include <d3d9.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Material dump: everything about the materials of one frame's 3D draws that the material map
// does not know, for building replacement materials outside the game. Written to
// roboblitz_remix_material_dumps\<time>.json next to the proxy, with the textures (top mip, as
// DDS named by content hash) in its textures\ folder. Pair it with a Remix capture of the same
// moment: a Remix material is keyed by the texture in stage 0, so `stage0Texture` matches it.

struct DumpedSampler {
    std::string name; // e.g. Texture2D_1
    IDirect3DBaseTexture9* texture = nullptr;
    DWORD addressU = 0, addressV = 0;
    bool srgb = false; // D3DSAMP_SRGBTEXTURE: the shader reads it converted to linear
};

struct DumpedDraw {
    uint64_t shaderHash = 0; // HashShaderBytecode of the pixel shader: the material map key
    std::vector<DumpedSampler> samplers;
    std::vector<std::pair<std::string, std::array<float, 4>>> constants; // pixel shader float constants
    std::string stage0;                         // sampler whose texture is in stage 0; "color"; empty if unknown
    IDirect3DBaseTexture9* stage0Texture = nullptr;
    bool alphaBlend = false;
};

class MaterialDump {
public:
    // Dumps the next frame
    void Request() { requested_ = true; }
    bool Active() const { return active_; }

    // At Present: writes the frame just finished if it was dumped, and starts a requested dump
    void EndFrame(unsigned frame);

    // A 3D draw of the frame being dumped
    void Add(const DumpedDraw& draw);

private:
    struct Material {
        DumpedDraw first;
        unsigned draws = 0;
        bool constantsVary = false; // later draws had other constant values
    };

    void Write(unsigned frame);

    bool requested_ = false;
    bool active_ = false;
    std::vector<Material> materials_;
};
