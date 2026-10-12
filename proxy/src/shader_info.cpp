#include "shader_info.h"

#include <cstdint>
#include <cstring>

namespace {

// Layout of the D3DX constant table, see D3DXSHADER_CONSTANTTABLE in d3dx9shader.h.
// All offsets are relative to the start of the table (right after the 'CTAB' FourCC).
struct CtabHeader {
    uint32_t size;
    uint32_t creator;
    uint32_t version;
    uint32_t constants;
    uint32_t constantInfo;
    uint32_t flags;
    uint32_t target;
};

struct CtabConstant {
    uint32_t name;
    uint16_t registerSet;
    uint16_t registerIndex;
    uint16_t registerCount;
    uint16_t reserved;
    uint32_t typeInfo;
    uint32_t defaultValue;
};

struct CtabType {
    uint16_t typeClass;
    uint16_t type;
    uint16_t rows;
    uint16_t columns;
    uint16_t elements;
    uint16_t structMembers;
    uint32_t structMemberInfo;
};

constexpr uint32_t kCommentOpcode = 0xFFFE;
constexpr uint32_t kCtabFourCC = 0x42415443; // 'CTAB'
constexpr uint16_t kRegisterSetFloat4 = 2;
constexpr uint16_t kRegisterSetSampler = 3;
constexpr uint16_t kClassMatrixRows = 2;
constexpr uint16_t kClassMatrixColumns = 3;

// Bounds-checked view of the constant table bytes
struct Table {
    const uint8_t* data;
    size_t size;

    template <typename T>
    const T* At(uint32_t offset) const {
        return offset + sizeof(T) <= size ? reinterpret_cast<const T*>(data + offset) : nullptr;
    }

    bool String(uint32_t offset, std::string& out) const {
        if (offset >= size) return false;
        const void* end = memchr(data + offset, 0, size - offset);
        if (!end) return false;
        out.assign(reinterpret_cast<const char*>(data + offset), static_cast<const uint8_t*>(end) - (data + offset));
        return true;
    }
};

// One entry of the constant table
struct Constant {
    std::string name;
    uint16_t registerSet;
    uint16_t registerIndex;
    uint16_t registerCount;
    const CtabType* type; // may be null
};

bool ReadConstants(const Table& table, std::vector<Constant>& out) {
    const CtabHeader* header = table.At<CtabHeader>(0);
    if (!header) return false;

    for (uint32_t i = 0; i < header->constants; ++i) {
        const CtabConstant* entry = table.At<CtabConstant>(header->constantInfo + i * sizeof(CtabConstant));
        if (!entry) return false;

        Constant constant{{}, entry->registerSet, entry->registerIndex, entry->registerCount,
                          table.At<CtabType>(entry->typeInfo)};
        if (!table.String(entry->name, constant.name)) return false;
        out.push_back(std::move(constant));
    }
    return true;
}

// Finds the constant table among the comments after the version token and reads it
bool ReadShaderConstants(const DWORD* function, std::vector<Constant>& out) {
    if (!function) return false;
    for (const DWORD* token = function + 1; (*token & 0xFFFF) == kCommentOpcode;) {
        const uint32_t length = (*token >> 16) & 0x7FFF; // in DWORDs, excluding this token
        if (length >= 1 && token[1] == kCtabFourCC) {
            const Table table{reinterpret_cast<const uint8_t*>(token + 2), (length - 1) * sizeof(DWORD)};
            return ReadConstants(table, out);
        }
        token += length + 1;
    }
    return false;
}

bool HasConstant(const std::vector<std::string>& names, const char* name) {
    for (const std::string& constant : names) {
        if (constant == name) return true;
    }
    return false;
}

} // namespace

const char* ToString(ShaderKind kind) {
    switch (kind) {
    case ShaderKind::World: return "World";
    case ShaderKind::UI: return "UI";
    case ShaderKind::Shadow: return "Shadow";
    case ShaderKind::Screen: return "Screen";
    case ShaderKind::Depth: return "Depth";
    case ShaderKind::Light: return "Light";
    }
    return "?";
}

int FindFloatRegister(const FloatRegisters& registers, const char* name) {
    for (const auto& [constantName, reg] : registers) {
        if (constantName == name) return reg;
    }
    return -1;
}

bool ParseVertexShader(const DWORD* function, VertexShaderInfo& out) {
    out = {};
    std::vector<Constant> constants;
    if (!ReadShaderConstants(function, constants)) return false;

    for (const Constant& constant : constants) {
        out.constants.push_back(constant.name);
        if (constant.registerSet == kRegisterSetFloat4) out.floatRegisters.emplace_back(constant.name, constant.registerIndex);

        const CtabType* type = constant.type;
        const bool isMatrix4x4 = type && constant.registerSet == kRegisterSetFloat4 &&
                                 (type->typeClass == kClassMatrixRows || type->typeClass == kClassMatrixColumns) &&
                                 type->rows == 4 && type->columns == 4 && constant.registerCount == 4;
        if (!isMatrix4x4) continue;

        MatrixConstant matrix;
        matrix.reg = constant.registerIndex;
        matrix.rowMajor = type->typeClass == kClassMatrixRows;

        if (constant.name == "ViewProjectionMatrix") out.viewProjection = matrix;
        else if (constant.name == "LocalToWorld") out.localToWorld = matrix;
    }

    const bool isLightPass = HasConstant(out.constants, "LightPositionAndInvRadius") || HasConstant(out.constants, "LightDirection");
    if (out.viewProjection.IsValid() && isLightPass) out.kind = ShaderKind::Light;
    else if (out.viewProjection.IsValid() && HasConstant(out.constants, "CameraPosition")) out.kind = ShaderKind::World;
    else if (out.viewProjection.IsValid()) out.kind = ShaderKind::Depth;
    else if (HasConstant(out.constants, "Transform")) out.kind = ShaderKind::UI;
    else if (HasConstant(out.constants, "ShadowMatrix")) out.kind = ShaderKind::Shadow;
    else out.kind = ShaderKind::Screen;
    return true;
}

bool ParsePixelShader(const DWORD* function, PixelShaderInfo& out) {
    out = {};
    std::vector<Constant> constants;
    if (!ReadShaderConstants(function, constants)) return false;

    for (const Constant& constant : constants) {
        out.constants.push_back(constant.name);
        if (constant.registerSet == kRegisterSetFloat4) out.floatRegisters.emplace_back(constant.name, constant.registerIndex);
        if (constant.registerSet != kRegisterSetSampler) continue;

        // Sampler arrays (e.g. LightMapTextures[3]) take one register per element
        for (uint16_t i = 0; i < constant.registerCount; ++i) {
            const size_t reg = constant.registerIndex + i;
            if (out.samplers.size() <= reg) out.samplers.resize(reg + 1);
            out.samplers[reg] = constant.registerCount > 1 ? constant.name + "[" + std::to_string(i) + "]"
                                                           : constant.name;
        }
    }
    return true;
}

uint64_t HashShaderBytecode(const DWORD* function) {
    if (!function || ((*function >> 8) & 0xFF) < 2) return 0;

    // Comments carry their length in bits 16-30, instructions (SM2+) in bits 24-27
    constexpr DWORD kEndToken = 0x0000FFFF;
    constexpr size_t kMaxTokens = 1 << 20;
    size_t tokens = 1;
    while (tokens < kMaxTokens && function[tokens] != kEndToken) {
        const DWORD token = function[tokens];
        tokens += 1 + ((token & 0xFFFF) == kCommentOpcode ? (token >> 16) & 0x7FFF : (token >> 24) & 0xF);
    }
    if (tokens >= kMaxTokens) return 0;
    ++tokens; // the end token

    uint64_t hash = 14695981039346656037ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(function);
    for (size_t i = 0; i < tokens * sizeof(DWORD); ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}
