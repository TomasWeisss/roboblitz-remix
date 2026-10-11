#pragma once

#include <windows.h>

#include <string>
#include <vector>

// What a vertex shader is used for, derived from the constants in its constant table.
enum class ShaderKind {
    World,   // 3D scene geometry: has ViewProjectionMatrix
    UI,      // canvas / HUD (SimpleElement): has Transform
    Shadow,  // shadow-map depth rendering: has ShadowMatrix
    Screen,  // full-screen passes: post-process, gamma, fog, ...
};

const char* ToString(ShaderKind kind);

// Location of a float4x4 shader constant.
struct MatrixConstant {
    int reg = -1;          // first constant register (c#), -1 if absent
    bool rowMajor = false; // HLSL default is column-major

    bool IsValid() const { return reg >= 0; }
};

struct VertexShaderInfo {
    ShaderKind kind = ShaderKind::Screen;
    MatrixConstant viewProjection;
    MatrixConstant localToWorld;
    std::vector<std::string> constants; // all constant names, for logging
};

// Parses the D3DX constant table (CTAB comment) of shader bytecode as passed to
// IDirect3DDevice9::CreateVertexShader. Returns false if the bytecode has no constant table.
bool ParseVertexShader(const DWORD* function, VertexShaderInfo& out);
