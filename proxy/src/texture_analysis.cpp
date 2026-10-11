#include "texture_analysis.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

constexpr UINT kMaxSamplesPerAxis = 64; // at most 64x64 blocks or pixels are sampled
constexpr unsigned kMinSamples = 8;
constexpr float kNormalFraction = 0.85f; // share of normal-like samples for a normal map
constexpr float kMaxMeanTilt = 0.25f;    // normal maps average to nearly straight up (x, y ~ 0)

struct Counter {
    unsigned samples = 0;
    unsigned normalLike = 0;
    float sumX = 0, sumY = 0;

    // A tangent-space normal stored as colour decodes to n = 2c - 1 with |n| = 1 and n.z > 0.
    // Colour textures rarely do: grey decodes to ~0, white or saturated colours to ~1.7.
    void Add(float r, float g, float b) {
        const float x = 2 * r - 1, y = 2 * g - 1, z = 2 * b - 1;
        const float length = std::sqrt(x * x + y * y + z * z);
        ++samples;
        sumX += x;
        sumY += y;
        if (length > 0.8f && length < 1.2f && z > 0.3f) ++normalLike;
    }

    void AddRgb565(uint16_t c) { Add(((c >> 11) & 31) / 31.0f, ((c >> 5) & 63) / 63.0f, (c & 31) / 31.0f); }

    TextureContent Result() const {
        if (samples < kMinSamples) return TextureContent::Unknown;
        // Surface detail tilts normals both ways, so a real normal map averages to (0, 0, 1).
        // This rejects blue colour textures (sky, water) whose texels also decode to unit length.
        const bool centered = std::fabs(sumX / samples) < kMaxMeanTilt && std::fabs(sumY / samples) < kMaxMeanTilt;
        const bool unitLength = normalLike >= kNormalFraction * samples;
        return centered && unitLength ? TextureContent::NormalMap : TextureContent::Color;
    }
};

UINT Step(UINT count) { return count > kMaxSamplesPerAxis ? count / kMaxSamplesPerAxis : 1; }

} // namespace

const char* ToString(TextureContent content) {
    switch (content) {
    case TextureContent::Unknown: return "unknown";
    case TextureContent::Color: return "color";
    case TextureContent::NormalMap: return "normal";
    }
    return "?";
}

TextureContent ClassifyTexture(D3DFORMAT format, UINT width, UINT height, const void* bits, INT pitch) {
    if (!bits || pitch <= 0 || width == 0 || height == 0) return TextureContent::Unknown;
    const auto* data = static_cast<const uint8_t*>(bits);
    Counter counter;

    switch (format) {
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5: {
        // 4x4 blocks; the colour part (two RGB565 endpoints first) is the whole DXT1 block and
        // the second half of DXT2-5 blocks
        const UINT blockSize = format == D3DFMT_DXT1 ? 8 : 16;
        const UINT colorOffset = format == D3DFMT_DXT1 ? 0 : 8;
        const UINT blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
        for (UINT by = 0; by < blocksY; by += Step(blocksY)) {
            for (UINT bx = 0; bx < blocksX; bx += Step(blocksX)) {
                const uint8_t* block = data + by * pitch + bx * blockSize + colorOffset;
                uint16_t endpoints[2];
                memcpy(endpoints, block, sizeof(endpoints));
                counter.AddRgb565(endpoints[0]);
                counter.AddRgb565(endpoints[1]);
            }
        }
        break;
    }
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        // Bytes in memory: B, G, R, A
        for (UINT y = 0; y < height; y += Step(height)) {
            for (UINT x = 0; x < width; x += Step(width)) {
                const uint8_t* texel = data + y * pitch + x * 4;
                counter.Add(texel[2] / 255.0f, texel[1] / 255.0f, texel[0] / 255.0f);
            }
        }
        break;
    default:
        return TextureContent::Unknown;
    }
    return counter.Result();
}
