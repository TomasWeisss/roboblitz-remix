// Tests for the game-independent proxy logic.
// Usage: proxy_tests [path to RoboGame\Content\LocalShaderCache.upk]

#include "camera.h"
#include "shader_info.h"

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

    std::printf("Shader cache: %d vertex shaders: World %d, UI %d, Shadow %d, Screen %d, Depth %d\n", parsed,
                kinds[ShaderKind::World], kinds[ShaderKind::UI], kinds[ShaderKind::Shadow], kinds[ShaderKind::Screen], kinds[ShaderKind::Depth]);
    std::printf("ViewProjectionMatrix registers:");
    for (const auto& [reg, count] : viewProjectionRegisters) std::printf(" c%d x%d", reg, count);
    std::printf("\n");
    CHECK(parsed == 184);
    CHECK(kinds[ShaderKind::World] == 125);
    CHECK(kinds[ShaderKind::Depth] == 23); // depth-only, velocity, occlusion query, shadow volume
}

} // namespace

int main(int argc, char** argv) {
    TestDecomposeUe3Camera();
    TestDecomposeScaledFinitePerspective();
    TestRejectOrthographic();
    TestReadMatrixConstant();
    if (argc > 1) TestShaderCache(argv[1]);

    std::printf(failures ? "%d check(s) failed\n" : "All tests passed\n", failures);
    return failures ? 1 : 0;
}
