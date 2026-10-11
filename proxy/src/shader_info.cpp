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

bool ParseTable(const Table& table, VertexShaderInfo& out) {
    const CtabHeader* header = table.At<CtabHeader>(0);
    if (!header) return false;

    for (uint32_t i = 0; i < header->constants; ++i) {
        const CtabConstant* constant = table.At<CtabConstant>(header->constantInfo + i * sizeof(CtabConstant));
        if (!constant) return false;

        std::string name;
        if (!table.String(constant->name, name)) return false;
        out.constants.push_back(name);

        const CtabType* type = table.At<CtabType>(constant->typeInfo);
        if (!type || constant->registerSet != kRegisterSetFloat4) continue;

        const bool isMatrix4x4 = (type->typeClass == kClassMatrixRows || type->typeClass == kClassMatrixColumns) &&
                                 type->rows == 4 && type->columns == 4 && constant->registerCount == 4;
        if (!isMatrix4x4) continue;

        MatrixConstant matrix;
        matrix.reg = constant->registerIndex;
        matrix.rowMajor = type->typeClass == kClassMatrixRows;

        if (name == "ViewProjectionMatrix") out.viewProjection = matrix;
        else if (name == "LocalToWorld") out.localToWorld = matrix;
    }
    return true;
}

bool HasConstant(const VertexShaderInfo& info, const char* name) {
    for (const std::string& constant : info.constants) {
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
    }
    return "?";
}

bool ParseVertexShader(const DWORD* function, VertexShaderInfo& out) {
    out = {};
    if (!function) return false;

    // Token 0 is the version; the constant table is one of the comments that follow it
    bool found = false;
    for (const DWORD* token = function + 1; (*token & 0xFFFF) == kCommentOpcode;) {
        const uint32_t length = (*token >> 16) & 0x7FFF; // in DWORDs, excluding this token
        if (length >= 1 && token[1] == kCtabFourCC) {
            const Table table{reinterpret_cast<const uint8_t*>(token + 2), (length - 1) * sizeof(DWORD)};
            found = ParseTable(table, out);
            break;
        }
        token += length + 1;
    }
    if (!found) return false;

    if (out.viewProjection.IsValid()) out.kind = ShaderKind::World;
    else if (HasConstant(out, "Transform")) out.kind = ShaderKind::UI;
    else if (HasConstant(out, "ShadowMatrix")) out.kind = ShaderKind::Shadow;
    else out.kind = ShaderKind::Screen;
    return true;
}
