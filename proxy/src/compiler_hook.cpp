// UE3 compiles its shaders at runtime with D3DXCompileShader, passing the material code generated
// from the material's expression graph as the include file Material.usf. Shaders loaded from the
// game's shader cache are never compiled again, hence the persistent material map.

#include "compiler_hook.h"

#include "config.h"
#include "log.h"
#include "material_albedo.h"
#include "shader_info.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <unknwn.h>

namespace {

// Minimal D3DX 9 declarations; the D3DX headers are not part of the Windows SDK
enum D3DXINCLUDE_TYPE { D3DXINC_LOCAL, D3DXINC_SYSTEM };

struct D3DXMACRO {
    LPCSTR Name;
    LPCSTR Definition;
};

struct ID3DXInclude {
    virtual HRESULT STDMETHODCALLTYPE Open(D3DXINCLUDE_TYPE type, LPCSTR fileName, LPCVOID parentData, LPCVOID* data,
                                           UINT* bytes) = 0;
    virtual HRESULT STDMETHODCALLTYPE Close(LPCVOID data) = 0;
};

struct ID3DXBuffer : IUnknown {
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize() = 0;
};

using D3DXCompileShaderFn = HRESULT(WINAPI*)(LPCSTR source, UINT sourceLength, const D3DXMACRO* defines,
                                             ID3DXInclude* include, LPCSTR function, LPCSTR profile, DWORD flags,
                                             ID3DXBuffer** shader, ID3DXBuffer** errors, IUnknown** constantTable);

D3DXCompileShaderFn originalCompileShader;

MaterialMap& Map() {
    static MaterialMap map;
    return map;
}

constexpr const char* kDiffuseFunction = "GetMaterialDiffuseColor";
constexpr const char* kEmissiveFunction = "GetMaterialEmissive";

// Forwards to the game's include handler and keeps a copy of the generated material code
class CapturingInclude final : public ID3DXInclude {
public:
    CapturingInclude(ID3DXInclude* inner, std::string& material) : inner_(inner), material_(material) {}

    HRESULT STDMETHODCALLTYPE Open(D3DXINCLUDE_TYPE type, LPCSTR fileName, LPCVOID parentData, LPCVOID* data,
                                   UINT* bytes) override {
        const HRESULT result = inner_->Open(type, fileName, parentData, data, bytes);
        if (SUCCEEDED(result) && data && *data && bytes) {
            std::string text(static_cast<const char*>(*data), *bytes);
            if (!ExtractMaterialExpression(text, kDiffuseFunction).empty()) material_ = std::move(text);
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override { return inner_->Close(data); }

private:
    ID3DXInclude* inner_;
    std::string& material_;
};

bool IsDefined(const D3DXMACRO* defines, const char* name) {
    for (; defines && defines->Name; ++defines) {
        if (strcmp(defines->Name, name) == 0) return !defines->Definition || strcmp(defines->Definition, "0") != 0;
    }
    return false;
}

void RecordMaterial(const std::string& material, const D3DXMACRO* defines, ID3DXBuffer* shader) {
    const uint64_t hash = HashShaderBytecode(static_cast<const DWORD*>(shader->GetBufferPointer()));
    if (!hash) return;

    const bool unlit = IsDefined(defines, "MATERIAL_LIGHTINGMODEL_UNLIT");
    MaterialMap::Entry entry{unlit ? "emissive" : "diffuse",
                             ExtractMaterialExpression(material, unlit ? kEmissiveFunction : kDiffuseFunction)};

    // A texture-less colour with a textured opacity (e.g. a vertex-coloured fog card with a mask):
    // the opacity texture carries the shape, and Remix takes alpha from stage 0
    if (AnalyzeAlbedo(entry.expression).sampler.empty()) {
        const bool translucent = IsDefined(defines, "MATERIALBLENDING_TRANSLUCENT") ||
                                 IsDefined(defines, "MATERIALBLENDING_ADDITIVE") ||
                                 IsDefined(defines, "MATERIALBLENDING_MODULATE");
        const bool masked = IsDefined(defines, "MATERIALBLENDING_MASKED");
        const char* opacityFunction = translucent ? "GetMaterialOpacity" : masked ? "GetMaterialMask" : nullptr;
        if (opacityFunction) {
            std::string opacity = ExtractMaterialExpression(material, opacityFunction);
            if (!AnalyzeAlbedo(opacity).sampler.empty()) entry = {translucent ? "opacity" : "mask", std::move(opacity)};
        }
    }

    if (Map().Add(hash, entry) && GetConfig().logShaders) {
        Log("Compiled material pixel shader %016llx: %s %s", static_cast<unsigned long long>(hash),
            entry.source.c_str(), entry.expression.c_str());
    }
}

HRESULT WINAPI HookCompileShader(LPCSTR source, UINT sourceLength, const D3DXMACRO* defines, ID3DXInclude* include,
                                 LPCSTR function, LPCSTR profile, DWORD flags, ID3DXBuffer** shader,
                                 ID3DXBuffer** errors, IUnknown** constantTable) {
    if (!profile || strncmp(profile, "ps_", 3) != 0) {
        return originalCompileShader(source, sourceLength, defines, include, function, profile, flags, shader, errors,
                                     constantTable);
    }

    std::string material;
    if (source) {
        std::string text(source, sourceLength);
        if (!ExtractMaterialExpression(text, kDiffuseFunction).empty()) material = std::move(text);
    }
    CapturingInclude capturing(include, material);
    const HRESULT result = originalCompileShader(source, sourceLength, defines, include ? &capturing : nullptr,
                                                 function, profile, flags, shader, errors, constantTable);
    if (SUCCEEDED(result) && shader && *shader && !material.empty()) RecordMaterial(material, defines, *shader);
    return result;
}

// Points `module`'s import of dll!function at `hook`, storing the previous target in `original`
// first. Returns false if the module does not import the function by name.
bool PatchImport(HMODULE module, const char* dll, const char* function, void* hook, void** original) {
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;

    for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         import->Name; ++import) {
        if (_stricmp(reinterpret_cast<const char*>(base + import->Name), dll) != 0 || !import->OriginalFirstThunk) {
            continue;
        }
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + import->OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + import->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), function) != 0) continue;

            *original = reinterpret_cast<void*>(addresses->u1.Function);
            DWORD protection;
            VirtualProtect(&addresses->u1.Function, sizeof(void*), PAGE_READWRITE, &protection);
            addresses->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&addresses->u1.Function, sizeof(void*), protection, &protection);
            return true;
        }
    }
    return false;
}

} // namespace

void InstallCompilerHook() {
    const size_t loaded = Map().Load(ModuleDirectory() + L"roboblitz_remix_materials.txt");
    const bool hooked = PatchImport(GetModuleHandleW(nullptr), "d3dx9_28.dll", "D3DXCompileShader",
                                    reinterpret_cast<void*>(&HookCompileShader),
                                    reinterpret_cast<void**>(&originalCompileShader));
    Log("Material map: %zu shaders loaded; D3DXCompileShader %s", loaded, hooked ? "hooked" : "import NOT FOUND");
}

const MaterialMap& GetMaterialMap() { return Map(); }
