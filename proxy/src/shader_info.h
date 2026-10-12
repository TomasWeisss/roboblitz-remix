#pragma once

#include <windows.h>

#include "material_albedo.h"

#include <cstdint>
#include <string>
#include <vector>

// What a vertex shader is used for, derived from the constants in its constant table.
enum class ShaderKind {
    World,   // 3D scene geometry: has ViewProjectionMatrix
    UI,      // canvas / HUD (SimpleElement): has Transform
    Shadow,  // shadow-map depth rendering: has ShadowMatrix
    Screen,  // full-screen passes: post-process, gamma, fog, ...
    Depth,   // depth-only, velocity, occlusion query: ViewProjectionMatrix but no CameraPosition
    Light,   // per-light additive passes: has LightPositionAndInvRadius or LightDirection
};

// Kinds whose draws get the camera transforms (3D geometry)
inline bool UsesCamera(ShaderKind kind) {
    return kind == ShaderKind::World || kind == ShaderKind::Depth || kind == ShaderKind::Light;
}

const char* ToString(ShaderKind kind);

// Location of a float4x4 shader constant.
struct MatrixConstant {
    int reg = -1;          // first constant register (c#), -1 if absent
    bool rowMajor = false; // HLSL default is column-major

    bool IsValid() const { return reg >= 0; }
};

// Float constants (c#) by name: (name, first register)
using FloatRegisters = std::vector<std::pair<std::string, int>>;

// First register of a float constant, or -1 if the shader does not use it
int FindFloatRegister(const FloatRegisters& registers, const char* name);

struct VertexShaderInfo {
    int id = 0; // assigned by the proxy, for logs
    ShaderKind kind = ShaderKind::Screen;
    MatrixConstant viewProjection;
    MatrixConstant localToWorld;
    FloatRegisters floatRegisters;
    std::vector<std::string> constants; // all constant names, for logging
};

struct PixelShaderInfo {
    int id = 0; // assigned by the proxy, for logs
    uint64_t hash = 0; // HashShaderBytecode: the material map key
    std::vector<std::string> samplers;  // sampler name by register (s#); empty if unused
    FloatRegisters floatRegisters;
    std::vector<std::string> constants; // all constant names, for logging

    // Albedo of the material the shader was compiled for, from the material map
    bool hasMaterial = false;      // the shader is in the material map
    std::string albedoExpression;  // for logs
    MaterialAlbedo albedo;
    int albedoRegister = -1;       // sampler register of albedo.sampler; -1 if absent or not sampled
};

// Parse the D3DX constant table (CTAB comment) of shader bytecode as passed to
// IDirect3DDevice9::CreateVertexShader / CreatePixelShader. Return false if the bytecode
// has no constant table.
bool ParseVertexShader(const DWORD* function, VertexShaderInfo& out);
bool ParsePixelShader(const DWORD* function, PixelShaderInfo& out);

// Hash (FNV-1a, 64 bit) of shader bytecode up to and including its end token. 0 for bytecode
// older than shader model 2, whose instruction lengths are not encoded.
uint64_t HashShaderBytecode(const DWORD* function);
