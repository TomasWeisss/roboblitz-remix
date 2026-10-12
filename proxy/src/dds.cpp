#include "dds.h"

#include <cstring>
#include <string>

namespace {

// DDS_PIXELFORMAT / DDS_HEADER (DirectXTex DDS.h)
struct DdsPixelFormat {
    uint32_t size = sizeof(DdsPixelFormat);
    uint32_t flags = 0;
    uint32_t fourCC = 0;
    uint32_t bitCount = 0;
    uint32_t masks[4] = {}; // R, G, B, A
};

struct DdsHeader {
    uint32_t size = sizeof(DdsHeader);
    uint32_t flags = 0;
    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t pitchOrLinearSize = 0;
    uint32_t depth = 0;
    uint32_t mipMapCount = 1;
    uint32_t reserved1[11] = {};
    DdsPixelFormat format;
    uint32_t caps = 0;
    uint32_t caps2 = 0;
    uint32_t caps3 = 0;
    uint32_t caps4 = 0;
    uint32_t reserved2 = 0;
};
static_assert(sizeof(DdsHeader) == 124, "DDS header layout");

constexpr uint32_t kFlagsTexture = 0x1 | 0x2 | 0x4 | 0x1000; // CAPS | HEIGHT | WIDTH | PIXELFORMAT
constexpr uint32_t kFlagPitch = 0x8;
constexpr uint32_t kFlagLinearSize = 0x80000;
constexpr uint32_t kCapsTexture = 0x1000;

constexpr uint32_t kPfAlphaPixels = 0x1;
constexpr uint32_t kPfAlpha = 0x2;
constexpr uint32_t kPfFourCC = 0x4;
constexpr uint32_t kPfRgb = 0x40;
constexpr uint32_t kPfLuminance = 0x20000;
constexpr uint32_t kPfBumpDuDv = 0x80000;

struct FormatInfo {
    D3DFORMAT format;
    const char* name;
    uint32_t bytesPerPixel; // 0 for block-compressed
    uint32_t pixelFlags;    // 0: FourCC is the D3DFORMAT number
    uint32_t masks[4];
};

// clang-format off
const FormatInfo kFormats[] = {
    {D3DFMT_DXT1, "DXT1", 0, kPfFourCC, {}},
    {D3DFMT_DXT2, "DXT2", 0, kPfFourCC, {}},
    {D3DFMT_DXT3, "DXT3", 0, kPfFourCC, {}},
    {D3DFMT_DXT4, "DXT4", 0, kPfFourCC, {}},
    {D3DFMT_DXT5, "DXT5", 0, kPfFourCC, {}},
    {D3DFMT_A8R8G8B8, "A8R8G8B8", 4, kPfRgb | kPfAlphaPixels, {0xFF0000, 0xFF00, 0xFF, 0xFF000000}},
    {D3DFMT_X8R8G8B8, "X8R8G8B8", 4, kPfRgb, {0xFF0000, 0xFF00, 0xFF, 0}},
    {D3DFMT_A8B8G8R8, "A8B8G8R8", 4, kPfRgb | kPfAlphaPixels, {0xFF, 0xFF00, 0xFF0000, 0xFF000000}},
    {D3DFMT_R5G6B5, "R5G6B5", 2, kPfRgb, {0xF800, 0x7E0, 0x1F, 0}},
    {D3DFMT_X1R5G5B5, "X1R5G5B5", 2, kPfRgb, {0x7C00, 0x3E0, 0x1F, 0}},
    {D3DFMT_A1R5G5B5, "A1R5G5B5", 2, kPfRgb | kPfAlphaPixels, {0x7C00, 0x3E0, 0x1F, 0x8000}},
    {D3DFMT_A4R4G4B4, "A4R4G4B4", 2, kPfRgb | kPfAlphaPixels, {0xF00, 0xF0, 0xF, 0xF000}},
    {D3DFMT_G16R16, "G16R16", 4, kPfRgb, {0xFFFF, 0xFFFF0000, 0, 0}},
    {D3DFMT_A8, "A8", 1, kPfAlpha, {0, 0, 0, 0xFF}},
    {D3DFMT_L8, "L8", 1, kPfLuminance, {0xFF, 0, 0, 0}},
    {D3DFMT_A8L8, "A8L8", 2, kPfLuminance | kPfAlphaPixels, {0xFF, 0, 0, 0xFF00}},
    {D3DFMT_L16, "L16", 2, kPfLuminance, {0xFFFF, 0, 0, 0}},
    {D3DFMT_V8U8, "V8U8", 2, kPfBumpDuDv, {0xFF, 0xFF00, 0, 0}},
    {D3DFMT_Q8W8V8U8, "Q8W8V8U8", 4, 0, {}},
    {D3DFMT_A16B16G16R16, "A16B16G16R16", 8, 0, {}},
    {D3DFMT_R16F, "R16F", 2, 0, {}},
    {D3DFMT_G16R16F, "G16R16F", 4, 0, {}},
    {D3DFMT_A16B16G16R16F, "A16B16G16R16F", 8, 0, {}},
    {D3DFMT_R32F, "R32F", 4, 0, {}},
    {D3DFMT_G32R32F, "G32R32F", 8, 0, {}},
    {D3DFMT_A32B32G32R32F, "A32B32G32R32F", 16, 0, {}},
};
// clang-format on

const FormatInfo* FindFormat(D3DFORMAT format) {
    for (const FormatInfo& info : kFormats) {
        if (info.format == format) return &info;
    }
    return nullptr;
}

} // namespace

bool GetLevelLayout(D3DFORMAT format, UINT width, UINT height, LevelLayout& out) {
    const FormatInfo* info = FindFormat(format);
    if (!info || width == 0 || height == 0) return false;
    if (info->bytesPerPixel == 0) {
        const size_t blockBytes = format == D3DFMT_DXT1 ? 8 : 16;
        out.rowBytes = ((width + 3) / 4) * blockBytes;
        out.rows = (height + 3) / 4;
    } else {
        out.rowBytes = size_t(width) * info->bytesPerPixel;
        out.rows = height;
    }
    return true;
}

const char* FormatName(D3DFORMAT format) {
    if (const FormatInfo* info = FindFormat(format)) return info->name;
    static thread_local std::string number;
    number = std::to_string(static_cast<int>(format));
    return number.c_str();
}

std::vector<uint8_t> MakeDds(D3DFORMAT format, UINT width, UINT height, const uint8_t* data, size_t size) {
    const FormatInfo* info = FindFormat(format);
    LevelLayout layout;
    if (!info || !GetLevelLayout(format, width, height, layout) || size < layout.Size()) return {};

    DdsHeader header;
    header.width = width;
    header.height = height;
    header.caps = kCapsTexture;
    const bool compressed = info->bytesPerPixel == 0;
    header.flags = kFlagsTexture | (compressed ? kFlagLinearSize : kFlagPitch);
    header.pitchOrLinearSize = static_cast<uint32_t>(compressed ? layout.Size() : layout.rowBytes);
    if (info->pixelFlags == 0 || info->pixelFlags == kPfFourCC) {
        header.format.flags = kPfFourCC;
        header.format.fourCC = static_cast<uint32_t>(format); // DXTn FourCCs; else the D3DFORMAT number
    } else {
        header.format.flags = info->pixelFlags;
        header.format.bitCount = info->bytesPerPixel * 8;
        memcpy(header.format.masks, info->masks, sizeof(header.format.masks));
    }

    std::vector<uint8_t> file(4 + sizeof(header) + layout.Size());
    memcpy(file.data(), "DDS ", 4);
    memcpy(file.data() + 4, &header, sizeof(header));
    memcpy(file.data() + 4 + sizeof(header), data, layout.Size());
    return file;
}
