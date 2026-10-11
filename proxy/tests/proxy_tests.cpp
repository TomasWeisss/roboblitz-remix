// Tests for the game-independent proxy logic.
// Usage: proxy_tests [path to RoboGame\Content\LocalShaderCache.upk]

#include "camera.h"
#include "shader_info.h"
#include "light_conversion.h"
#include "texture_analysis.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

bool Near(const D3DMATRIX& a, const D3DMATRIX& b, float tolerance) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (std::fabs(a.m[r][c] - b.m[r][c]) > tolerance) return false;
        }
    }
    return true;
}

// UE3-style camera: FViewMatrix swaps axes (UE X forward, Y right, Z up -> view z, x, y)
// and FPerspectiveMatrix has an infinite far plane.
D3DMATRIX MakeView(float yaw, float pitch, const float position[3]) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float forward[3] = {cp * cy, cp * sy, sp};
    const float right[3] = {-sy, cy, 0};
    const float up[3] = {-sp * cy, -sp * sy, cp};
    const float* axes[3] = {right, up, forward}; // view x, y, z
    D3DMATRIX v = {};
    for (int k = 0; k < 3; ++k) {
        for (int r = 0; r < 3; ++r) v.m[r][k] = axes[k][r];
        v.m[3][k] = -(axes[k][0] * position[0] + axes[k][1] * position[1] + axes[k][2] * position[2]);
    }
    v._44 = 1;
    return v;
}

D3DMATRIX MakeUe3Projection(float halfFov, float aspect, float nearPlane) {
    D3DMATRIX p = {};
    p._11 = 1.0f / std::tan(halfFov);
    p._22 = aspect / std::tan(halfFov);
    p._33 = 1.0f;
    p._34 = 1.0f;
    p._43 = -nearPlane;
    return p;
}

void TestDecomposeUe3Camera() {
    const float position[3] = {1200.0f, -340.0f, 96.0f};
    const D3DMATRIX view = MakeView(0.7f, -0.25f, position);
    const D3DMATRIX projection = MakeUe3Projection(0.785f, 16.0f / 9.0f, 10.0f);
    const D3DMATRIX viewProjection = Multiply(view, projection);

    ViewProjection result;
    CHECK(DecomposeViewProjection(viewProjection, result));
    CHECK(result.error < 1e-4f);
    CHECK(Near(result.view, view, 1e-3f));
    CHECK(Near(result.projection, projection, 1e-4f));
    CHECK(std::fabs(result.handedness - 1.0f) < 1e-4f);
    CHECK(std::fabs(result.nearPlane - 10.0f) < 1e-3f);
    CHECK(std::isinf(result.farPlane));
}

void TestDecomposeScaledFinitePerspective() {
    // A finite far plane, off-center projection, and an overall scale of clip space
    const float position[3] = {0.0f, 50.0f, -20.0f};
    const D3DMATRIX view = MakeView(-2.1f, 0.4f, position);
    D3DMATRIX projection = {};
    projection._11 = 1.2f;
    projection._22 = 2.0f;
    projection._31 = 0.05f;
    projection._32 = -0.02f;
    projection._33 = 1000.0f / (1000.0f - 5.0f);
    projection._34 = 1.0f;
    projection._43 = -5.0f * 1000.0f / (1000.0f - 5.0f);

    D3DMATRIX scaled = Multiply(view, projection);
    for (auto& row : scaled.m) {
        for (float& v : row) v *= 3.0f;
    }

    ViewProjection result;
    CHECK(DecomposeViewProjection(scaled, result));
    CHECK(result.error < 1e-4f);
    CHECK(Near(result.view, view, 1e-3f));
    CHECK(Near(result.projection, projection, 1e-4f));
    CHECK(std::fabs(result.nearPlane - 5.0f) < 1e-2f);
    CHECK(std::fabs(result.farPlane - 1000.0f) < 1.0f);
}

void TestRejectOrthographic() {
    ViewProjection result;
    CHECK(!DecomposeViewProjection(Identity(), result));
}

void TestReadMatrixConstant() {
    // Column-major registers are rows of the D3D row-vector matrix
    const float registers[4][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16}};
    const D3DMATRIX columns = ReadMatrixConstant(registers, false);
    const D3DMATRIX rows = ReadMatrixConstant(registers, true);
    CHECK(columns._12 == 2 && columns._21 == 5 && columns._43 == 15);
    CHECK(rows._12 == 5 && rows._21 == 2 && rows._43 == 12);
}

// RGB565 helper for synthetic DXT blocks
uint16_t Rgb565(float r, float g, float b) {
    return static_cast<uint16_t>((lround(r * 31) << 11) | (lround(g * 63) << 5) | lround(b * 31));
}

// A DXT1 texture where every block has the given two endpoint colours
std::vector<uint8_t> MakeDxt1(UINT blocksX, UINT blocksY, uint16_t c0, uint16_t c1) {
    std::vector<uint8_t> data(blocksX * blocksY * 8, 0);
    for (size_t i = 0; i < data.size(); i += 8) {
        memcpy(&data[i], &c0, 2);
        memcpy(&data[i + 2], &c1, 2);
    }
    return data;
}

void TestClassifyTextures() {
    // Flat and slightly tilted normals vs typical colours
    const auto normal = MakeDxt1(16, 16, Rgb565(0.5f, 0.5f, 1.0f), Rgb565(0.62f, 0.45f, 0.96f));
    const auto grey = MakeDxt1(16, 16, Rgb565(0.5f, 0.5f, 0.5f), Rgb565(0.4f, 0.4f, 0.4f));
    const auto sky = MakeDxt1(16, 16, Rgb565(0.35f, 0.55f, 0.9f), Rgb565(0.2f, 0.4f, 0.8f));
    CHECK(ClassifyTexture(D3DFMT_DXT1, 64, 64, normal.data(), 16 * 8) == TextureContent::NormalMap);
    CHECK(ClassifyTexture(D3DFMT_DXT1, 64, 64, grey.data(), 16 * 8) == TextureContent::Color);
    CHECK(ClassifyTexture(D3DFMT_DXT1, 64, 64, sky.data(), 16 * 8) == TextureContent::Color);

    // DXT5: colour endpoints are in the second half of each 16-byte block
    std::vector<uint8_t> dxt5(16 * 16 * 16, 0xFF);
    for (size_t i = 0; i < dxt5.size(); i += 16) {
        const uint16_t c = Rgb565(0.5f, 0.5f, 1.0f);
        memcpy(&dxt5[i + 8], &c, 2);
        memcpy(&dxt5[i + 10], &c, 2);
    }
    CHECK(ClassifyTexture(D3DFMT_DXT5, 64, 64, dxt5.data(), 16 * 16) == TextureContent::NormalMap);

    // A8R8G8B8, bytes B G R A
    std::vector<uint8_t> argb(32 * 32 * 4);
    for (size_t i = 0; i < argb.size(); i += 4) {
        argb[i] = 255; argb[i + 1] = 128; argb[i + 2] = 128; argb[i + 3] = 255;
    }
    CHECK(ClassifyTexture(D3DFMT_A8R8G8B8, 32, 32, argb.data(), 32 * 4) == TextureContent::NormalMap);
    for (size_t i = 0; i < argb.size(); i += 4) argb[i] = 40; // brownish: B=40 G=128 R=128
    CHECK(ClassifyTexture(D3DFMT_A8R8G8B8, 32, 32, argb.data(), 32 * 4) == TextureContent::Color);

    // Too small to judge, and unsupported formats
    CHECK(ClassifyTexture(D3DFMT_DXT1, 4, 4, normal.data(), 8) == TextureContent::Unknown);
    CHECK(ClassifyTexture(D3DFMT_R5G6B5, 64, 64, normal.data(), 128) == TextureContent::Unknown);
}

void TestLightConversion() {
    const float positionAndInvRadius[4] = {100, 200, 300, 1.0f / 512};
    const float color[3] = {4, 2, 1}; // brightness 4

    D3DLIGHT9 point;
    CHECK(ToD3DLight(MakePointLight(positionAndInvRadius, color), point));
    CHECK(point.Type == D3DLIGHT_POINT);
    CHECK(point.Position.x == 100 && point.Position.y == 200 && point.Position.z == 300);
    CHECK(std::fabs(point.Range - 512 * 2) < 1e-2f); // radius * sqrt(brightness)
    CHECK(point.Attenuation0 == 1 && point.Attenuation1 == 0 && point.Attenuation2 == 0);

    // UE3 spot: outer cone 40 deg, inner 20 deg (half angles)
    const float pi = 3.14159265f;
    const float cosOuter = std::cos(40 * pi / 180), cosInner = std::cos(20 * pi / 180);
    const float spotAngles[2] = {cosOuter, 1.0f / (cosInner - cosOuter)};
    const float spotDirection[3] = {0, 0, -2}; // pointing down, not normalized
    D3DLIGHT9 spot;
    CHECK(ToD3DLight(MakeSpotLight(positionAndInvRadius, color, spotDirection, spotAngles), spot));
    CHECK(spot.Type == D3DLIGHT_SPOT);
    CHECK(std::fabs(spot.Direction.z + 1) < 1e-5f);
    CHECK(std::fabs(spot.Phi - 80 * pi / 180) < 1e-3f);   // D3D9 uses full cone angles
    CHECK(std::fabs(spot.Theta - 40 * pi / 180) < 1e-3f);

    // Directional: UE3 gives the direction towards the light, D3D9 the direction it travels
    const float towardsLight[3] = {0, 0, 1};
    D3DLIGHT9 sun;
    CHECK(ToD3DLight(MakeDirectionalLight(towardsLight, color), sun));
    CHECK(sun.Type == D3DLIGHT_DIRECTIONAL && sun.Direction.z == -1);

    // Same light from another light pass vs a different light; black lights are dropped
    const float moved[4] = {100.2f, 200, 300, 1.0f / 512};
    const float elsewhere[4] = {150, 200, 300, 1.0f / 512};
    CHECK(SameLight(MakePointLight(positionAndInvRadius, color), MakePointLight(moved, color)));
    CHECK(!SameLight(MakePointLight(positionAndInvRadius, color), MakePointLight(elsewhere, color)));
    const float black[3] = {0, 0, 0};
    CHECK(!ToD3DLight(MakePointLight(positionAndInvRadius, black), point));
}

// Runs the parser over every vs_3_0 shader in the game's shader cache
void TestShaderCache(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("FAILED: cannot open %s\n", path);
        ++failures;
        return;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    std::map<ShaderKind, int> kinds;
    std::map<int, int> viewProjectionRegisters;
    int parsed = 0;
    const DWORD vs30 = 0xFFFE0300;
    for (size_t i = 0; i + 8 <= bytes.size(); ++i) {
        if (memcmp(&bytes[i], &vs30, 4) != 0) continue;
        // Copy to aligned memory, as the D3D runtime would see it
        const size_t dwords = (bytes.size() - i) / 4;
        std::vector<DWORD> function(dwords);
        memcpy(function.data(), &bytes[i], dwords * 4);

        VertexShaderInfo info;
        if (!ParseVertexShader(function.data(), info)) continue;
        ++parsed;
        ++kinds[info.kind];
        if (info.viewProjection.IsValid()) ++viewProjectionRegisters[info.viewProjection.reg];
        CHECK(!info.viewProjection.rowMajor);
    }

    std::printf("Shader cache: %d vertex shaders: World %d, UI %d, Shadow %d, Screen %d, Depth %d, Light %d\n", parsed,
                kinds[ShaderKind::World], kinds[ShaderKind::UI], kinds[ShaderKind::Shadow], kinds[ShaderKind::Screen],
                kinds[ShaderKind::Depth], kinds[ShaderKind::Light]);
    std::printf("ViewProjectionMatrix registers:");
    for (const auto& [reg, count] : viewProjectionRegisters) std::printf(" c%d x%d", reg, count);
    std::printf("\n");
    CHECK(parsed == 184);
    CHECK(kinds[ShaderKind::World] == 56);  // base pass, translucency, particles, decals
    CHECK(kinds[ShaderKind::Depth] == 23);  // depth-only, velocity, occlusion query, shadow volume
    CHECK(kinds[ShaderKind::Light] == 69);  // point/spot/directional passes for every vertex factory

    // Pixel shaders: what does sampler 0 hold? (Remix uses stage 0 as albedo)
    std::map<std::string, int> sampler0;
    int pixelShaders = 0;
    const DWORD ps30 = 0xFFFF0300;
    for (size_t i = 0; i + 8 <= bytes.size(); ++i) {
        if (memcmp(&bytes[i], &ps30, 4) != 0) continue;
        const size_t dwords = (bytes.size() - i) / 4;
        std::vector<DWORD> function(dwords);
        memcpy(function.data(), &bytes[i], dwords * 4);

        PixelShaderInfo info;
        if (!ParsePixelShader(function.data(), info)) continue;
        ++pixelShaders;
        ++sampler0[info.samplers.empty() || info.samplers[0].empty() ? "(none)" : info.samplers[0]];
    }
    std::printf("Shader cache: %d pixel shaders; sampler 0 holds:", pixelShaders);
    for (const auto& [name, count] : sampler0) std::printf(" %s x%d", name.c_str(), count);
    std::printf("\n");
    CHECK(pixelShaders > 1000);
}

} // namespace

int main(int argc, char** argv) {
    TestDecomposeUe3Camera();
    TestDecomposeScaledFinitePerspective();
    TestRejectOrthographic();
    TestReadMatrixConstant();
    TestClassifyTextures();
    TestLightConversion();
    if (argc > 1) TestShaderCache(argv[1]);

    std::printf(failures ? "%d check(s) failed\n" : "All tests passed\n", failures);
    return failures ? 1 : 0;
}
