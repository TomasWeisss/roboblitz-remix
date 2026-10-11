// The game draws everything with vertex shaders and never calls SetTransform, so Remix has no
// camera. Before each draw we look at the bound vertex shader's constant table, read the camera
// matrix from its constants, and hand Remix fixed-function transforms built from it.
//
// Hooks patch the vtables of the real IDirect3D9 / IDirect3DDevice9 (the Remix bridge client).

#include "hooks.h"

#include "camera.h"
#include "config.h"
#include "log.h"
#include "shader_info.h"

#include <cstdio>
#include <cstring>
#include <d3d9.h>
#include <string>
#include <unordered_map>
#include <winerror.h>
#include <winnt.h>

namespace {

// vtable slots, in declaration order of d3d9.h
namespace slot {
    constexpr int CreateDevice = 16;              // IDirect3D9
    constexpr int Reset = 16;                     // IDirect3DDevice9 ...
    constexpr int Present = 17;
    constexpr int StretchRect = 34;
    constexpr int SetRenderTarget = 37;
    constexpr int Clear = 43;
    constexpr int DrawPrimitive = 81;
    constexpr int DrawIndexedPrimitive = 82;
    constexpr int DrawPrimitiveUP = 83;
    constexpr int DrawIndexedPrimitiveUP = 84;
    constexpr int CreateVertexShader = 91;
    constexpr int SetVertexShader = 92;
    constexpr int SetVertexShaderConstantF = 94;
    constexpr int CreateQuery = 118;

    constexpr int QueryIssue = 6;
}

template <typename Fn>
void PatchVtable(void* object, int index, Fn hook, Fn& original) {
    void** vtable = *static_cast<void***>(object);
    if (vtable[index] == reinterpret_cast<void*>(hook)) return; // shared vtable, already patched
    DWORD protection;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &protection);
    original = reinterpret_cast<Fn>(vtable[index]);
    vtable[index] = reinterpret_cast<void*>(hook);
    VirtualProtect(&vtable[index], sizeof(void*), protection, &protection);
}

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                    IDirect3DDevice9**);
using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using StretchRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*,
                                                   const RECT*, D3DTEXTUREFILTERTYPE);
using SetRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using ClearFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
using DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT,
                                                            UINT);
using DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                                              const void*, D3DFORMAT, const void*, UINT);
using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetVertexShaderConstantFFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using CreateQueryFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
using QueryIssueFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DQuery9*, DWORD);

CreateDeviceFn originalCreateDevice;
ResetFn originalReset;
PresentFn originalPresent;
StretchRectFn originalStretchRect;
SetRenderTargetFn originalSetRenderTarget;
ClearFn originalClear;
DrawPrimitiveFn originalDrawPrimitive;
DrawIndexedPrimitiveFn originalDrawIndexedPrimitive;
DrawPrimitiveUPFn originalDrawPrimitiveUP;
DrawIndexedPrimitiveUPFn originalDrawIndexedPrimitiveUP;
CreateVertexShaderFn originalCreateVertexShader;
SetVertexShaderFn originalSetVertexShader;
SetVertexShaderConstantFFn originalSetVertexShaderConstantF;
CreateQueryFn originalCreateQuery;
QueryIssueFn originalQueryIssue;

constexpr int kKindCount = 5;
constexpr UINT kMaxVertexConstants = 256; // vs_3_0

struct DrawStats {
    unsigned drawn[kKindCount] = {};
    unsigned skipped[kKindCount] = {};
    unsigned fixedFunction = 0;
    unsigned badCamera = 0;
};

// The game uses a single device on one thread
struct DeviceState {
    float vsConstants[kMaxVertexConstants][4] = {};
    std::unordered_map<IDirect3DVertexShader9*, VertexShaderInfo> shaders;
    const VertexShaderInfo* currentShader = nullptr;

    // Last transforms handed to the device, to avoid redundant calls through the bridge
    D3DMATRIX world, view, projection;
    bool transformsKnown = false;

    // Last decomposed camera
    D3DMATRIX cameraInput;
    ViewProjection camera;
    bool cameraValid = false;
    bool cameraLogged = false;

    // Render target tracking
    IDirect3DSurface9* backBuffer = nullptr;
    bool renderingToBackbuffer = true;

    unsigned activeOcclusionQueries = 0;

    DrawStats stats;
    unsigned frame = 0;

    // Frame dump: logs every draw of one frame, merging consecutive identical lines
    bool dumping = false;
    std::string dumpLine;
    unsigned dumpRepeat = 0;
    unsigned dumpPrimitives = 0;
} state;

void RememberBackbuffer(IDirect3DDevice9* device) {
    IDirect3DSurface9* surface = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)) && surface) {
        state.backBuffer = surface;
        surface->Release();
    }
    
    state.renderingToBackbuffer = true;
}

// --- Frame dump ---

constexpr int kDumpKey = VK_F11; // with Ctrl

void FlushDumpLine() {
    if (state.dumpRepeat > 0) {
        Log("  %ux %s (%u primitives)", state.dumpRepeat, state.dumpLine.c_str(), state.dumpPrimitives);
    }
    state.dumpRepeat = 0;
    state.dumpPrimitives = 0;
}

void DumpEvent(const std::string& line, unsigned primitives) {
    if (line != state.dumpLine) {
        FlushDumpLine();
        state.dumpLine = line;
    }
    ++state.dumpRepeat;
    state.dumpPrimitives += primitives;
}

std::string DescribeSurface(IDirect3DDevice9* device, IDirect3DSurface9* surface) {
    if (!surface) return "null";
    D3DSURFACE_DESC desc = {};
    surface->GetDesc(&desc);

    bool isBackBuffer = false;
    IDirect3DSurface9* backBuffer = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer) {
        isBackBuffer = backBuffer == surface;
        backBuffer->Release();
    }
    char text[96];
    snprintf(text, sizeof(text), "%s%p %ux%u fmt %d", isBackBuffer ? "BACKBUFFER " : "", static_cast<void*>(surface),
             desc.Width, desc.Height, static_cast<int>(desc.Format));
    return text;
}

std::string DescribeRenderTarget(IDirect3DDevice9* device) {
    IDirect3DSurface9* target = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || !target) return "null";
    std::string text = DescribeSurface(device, target);
    target->Release();
    return text;
}

bool SameMatrix(const D3DMATRIX& a, const D3DMATRIX& b) { return memcmp(&a, &b, sizeof(D3DMATRIX)) == 0; }

void LogMatrix(const char* name, const D3DMATRIX& m) {
    Log("  %s:", name);
    for (int r = 0; r < 4; ++r) Log("    %12.5f %12.5f %12.5f %12.5f", m.m[r][0], m.m[r][1], m.m[r][2], m.m[r][3]);
}

void SetTransforms(IDirect3DDevice9* device, const D3DMATRIX& world, const D3DMATRIX& view,
                   const D3DMATRIX& projection) {
    const bool known = state.transformsKnown;
    if (!known || !SameMatrix(world, state.world)) device->SetTransform(D3DTS_WORLD, &world);
    if (!known || !SameMatrix(view, state.view)) device->SetTransform(D3DTS_VIEW, &view);
    if (!known || !SameMatrix(projection, state.projection)) device->SetTransform(D3DTS_PROJECTION, &projection);
    state.world = world;
    state.view = view;
    state.projection = projection;
    state.transformsKnown = true;
}

// Decomposes the camera matrix, reusing the previous result while it does not change
bool UpdateCamera(const D3DMATRIX& viewProjection) {
    if (state.cameraValid && SameMatrix(viewProjection, state.cameraInput)) return true;

    state.cameraInput = viewProjection;
    state.cameraValid = DecomposeViewProjection(viewProjection, state.camera) && state.camera.error < 1e-3f;

    if (!state.cameraLogged) {
        state.cameraLogged = true;
        const ViewProjection& c = state.camera;
        Log("First camera: valid=%d error=%g handedness=%g near=%g far=%g", state.cameraValid, c.error, c.handedness,
            c.nearPlane, c.farPlane);
        LogMatrix("ViewProjectionMatrix", viewProjection);
        LogMatrix("View", c.view);
        LogMatrix("Projection", c.projection);
    }
    return state.cameraValid;
}

// Sets up transforms for the next draw. Returns false if the draw should be skipped.
bool PrepareDraw(IDirect3DDevice9* device) {
    const VertexShaderInfo* shader = state.currentShader;
    if (!shader) {
        ++state.stats.fixedFunction; // fixed-function draw: the game's own transforms apply
        return true;
    }

    const Config& config = GetConfig();
    ShaderKind effectiveKind = shader->kind;
    if (shader->kind == ShaderKind::UI && !state.renderingToBackbuffer)
        effectiveKind = ShaderKind::Screen;
    bool draw = true;

    // Always draw occlusion queries
    if (state.activeOcclusionQueries > 0)
        return draw;

    switch (effectiveKind) {
        case ShaderKind::Depth:
            if (config.skipDepthPasses) {
                draw = false;
                break;
            }
            [[fallthrough]];
        case ShaderKind::World: {
            const MatrixConstant& vp = shader->viewProjection;
            if (!UpdateCamera(ReadMatrixConstant(&state.vsConstants[vp.reg], vp.rowMajor))) {
                ++state.stats.badCamera;
                break;
            }
            const MatrixConstant& l2w = shader->localToWorld;
            const D3DMATRIX world = config.useLocalToWorld && l2w.IsValid()
                                        ? ReadMatrixConstant(&state.vsConstants[l2w.reg], l2w.rowMajor)
                                        : Identity();
            SetTransforms(device, world, state.camera.view, state.camera.projection);
            break;
        }
        case ShaderKind::Shadow:
            draw = !config.skipShadowPasses;
            break;
        case ShaderKind::Screen:
            draw = !config.skipScreenPasses;
            break;
        case ShaderKind::UI:
            break;
    }

    // Everything except world geometry is screen-space: an identity projection is orthographic,
    // so Remix treats it as UI (rtx.orthographicIsUI) instead of trying to ray trace it
    if (draw && effectiveKind != ShaderKind::World) 
        SetTransforms(device, Identity(), Identity(), Identity());

    (draw ? state.stats.drawn : state.stats.skipped)[static_cast<int>(effectiveKind)]++;
    return draw;
}

// PrepareDraw plus frame-dump logging
bool BeforeDraw(IDirect3DDevice9* device, unsigned primitives) {
    const bool draw = PrepareDraw(device);
    if (state.dumping) {
        const VertexShaderInfo* shader = state.currentShader;
        char text[64];
        snprintf(text, sizeof(text), "%s %s VS %p -> ", draw ? "draw" : "SKIP",
                 shader ? ToString(shader->kind) : "fixed-function", static_cast<const void*>(shader));
        DumpEvent(text + DescribeRenderTarget(device), primitives);
    }
    return draw;
}

void LogFrameStats() {
    const DrawStats& s = state.stats;
    Log("Frame %u draws: World %u, UI %u, Shadow %u (skipped %u), Screen %u (skipped %u), fixed-function %u, "
        "bad camera %u",
        state.frame, s.drawn[0], s.drawn[1], s.drawn[2], s.skipped[2], s.drawn[3], s.skipped[3], s.fixedFunction,
        s.badCamera);
}

// --- Device hooks ---

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    state.transformsKnown = false; // Reset restores default device state
    auto reset = originalReset(device, params);

    // Reset invalidates swapchain
    if (SUCCEEDED(reset))
        RememberBackbuffer(device);
    
    return reset;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window,
                                      const RGNDATA* dirty) {
    ++state.frame;
    const unsigned f = state.frame;
    if (f == 1 || f == 10 || f == 100 || f == 1000 || f % 5000 == 0) LogFrameStats();
    state.stats = {};

    if (state.dumping) {
        FlushDumpLine();
        Log("End of frame dump");
        state.dumping = false;
    }
    // Dump the next frame at the configured frame number, or on Ctrl+F11
    const bool hotkey = (GetAsyncKeyState(kDumpKey) & 1) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
    if (hotkey || f == GetConfig().dumpFrame) {
        Log("Frame dump of frame %u (draw/SKIP, shader kind, vertex shader, render target):", f + 1);
        state.dumpLine.clear();
        state.dumping = true;
    }
    return originalPresent(device, source, dest, window, dirty);
}

HRESULT STDMETHODCALLTYPE HookStretchRect(IDirect3DDevice9* device, IDirect3DSurface9* source, const RECT* sourceRect,
                                          IDirect3DSurface9* dest, const RECT* destRect, D3DTEXTUREFILTERTYPE filter) {
    if (state.dumping) {
        DumpEvent("StretchRect " + DescribeSurface(device, source) + " -> " + DescribeSurface(device, dest), 0);
    }
    return originalStretchRect(device, source, sourceRect, dest, destRect, filter);
}

HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* device, DWORD targetIndex, IDirect3DSurface9* target) {
    if (targetIndex == 0)
        state.renderingToBackbuffer = target == state.backBuffer;
    
    return originalSetRenderTarget(device, targetIndex, target);
}

HRESULT STDMETHODCALLTYPE HookClear(IDirect3DDevice9* device, DWORD count, const D3DRECT* rects, DWORD flags,
                                    D3DCOLOR color, float z, DWORD stencil) {
    if (state.dumping) {
        char text[48];
        snprintf(text, sizeof(text), "Clear flags %lu -> ", flags);
        DumpEvent(text + DescribeRenderTarget(device), 0);
    }
    return originalClear(device, count, rects, flags, color, z, stencil);
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT start, UINT count) {
    return BeforeDraw(device, count) ? originalDrawPrimitive(device, type, start, count) : D3D_OK;
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT baseVertex,
                                                   UINT minIndex, UINT numVertices, UINT startIndex, UINT count) {
    return BeforeDraw(device, count)
               ? originalDrawIndexedPrimitive(device, type, baseVertex, minIndex, numVertices, startIndex, count)
               : D3D_OK;
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count,
                                              const void* data, UINT stride) {
    return BeforeDraw(device, count) ? originalDrawPrimitiveUP(device, type, count, data, stride) : D3D_OK;
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minIndex,
                                                     UINT numVertices, UINT count, const void* indices,
                                                     D3DFORMAT indexFormat, const void* vertices, UINT stride) {
    return BeforeDraw(device, count) ? originalDrawIndexedPrimitiveUP(device, type, minIndex, numVertices, count, indices,
                                                                indexFormat, vertices, stride)
                               : D3D_OK;
}

HRESULT STDMETHODCALLTYPE HookCreateVertexShader(IDirect3DDevice9* device, const DWORD* function,
                                                 IDirect3DVertexShader9** shader) {
    const HRESULT result = originalCreateVertexShader(device, function, shader);
    if (FAILED(result) || !shader || !*shader) return result;

    VertexShaderInfo info;
    if (!ParseVertexShader(function, info)) {
        Log("Vertex shader %p has no constant table; treating it as Screen", static_cast<void*>(*shader));
    }

    if (GetConfig().logShaders) {
        std::string names;
        for (const std::string& name : info.constants) names += " " + name;
        Log("Vertex shader %p: %s, ViewProjectionMatrix c%d, LocalToWorld c%d, constants:%s",
            static_cast<void*>(*shader), ToString(info.kind), info.viewProjection.reg, info.localToWorld.reg,
            names.c_str());
    }

    // A released shader's address can be reused by a new one, so always overwrite
    state.shaders[*shader] = std::move(info);
    return result;
}

HRESULT STDMETHODCALLTYPE HookSetVertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9* shader) {
    if (!shader) {
        state.currentShader = nullptr;
    } else {
        const auto it = state.shaders.find(shader);
        state.currentShader = it != state.shaders.end() ? &it->second : nullptr;
    }
    return originalSetVertexShader(device, shader);
}

HRESULT STDMETHODCALLTYPE HookSetVertexShaderConstantF(IDirect3DDevice9* device, UINT start, const float* data,
                                                       UINT count) {
    if (data && start < kMaxVertexConstants) {
        const UINT copied = count < kMaxVertexConstants - start ? count : kMaxVertexConstants - start;
        memcpy(state.vsConstants[start], data, copied * sizeof(float[4]));
    }
    return originalSetVertexShaderConstantF(device, start, data, count);
}

HRESULT STDMETHODCALLTYPE HookQueryIssue(IDirect3DQuery9* query, DWORD flags) {
    if (query->GetType() == D3DQUERYTYPE_OCCLUSION) {
        if (flags & D3DISSUE_BEGIN)
            ++state.activeOcclusionQueries;
        if ((flags & D3DISSUE_END) && state.activeOcclusionQueries > 0)
            --state.activeOcclusionQueries;
    }

    return originalQueryIssue(query, flags);
}

HRESULT STDMETHODCALLTYPE HookCreateQuery(IDirect3DDevice9* device, D3DQUERYTYPE type, IDirect3DQuery9** query) {
    const HRESULT result = originalCreateQuery(device, type, query);
    if (SUCCEEDED(result) && query && *query)
        PatchVtable(*query, slot::QueryIssue, HookQueryIssue, originalQueryIssue);

    return result;
}

// --- IDirect3D9 hook ---

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD flags,
                                           D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** device) {
    const HRESULT result = originalCreateDevice(d3d, adapter, type, window, flags, params, device);
    if (FAILED(result) || !device || !*device) {
        Log("CreateDevice failed: 0x%08lx", result);
        return result;
    }
    Log("CreateDevice: %ux%u, windowed=%d", params ? params->BackBufferWidth : 0,
        params ? params->BackBufferHeight : 0, params ? params->Windowed : -1);

    state = DeviceState{};
    IDirect3DDevice9* d = *device;
    RememberBackbuffer(d);
    PatchVtable(d, slot::Reset, HookReset, originalReset);
    PatchVtable(d, slot::Present, HookPresent, originalPresent);
    PatchVtable(d, slot::StretchRect, HookStretchRect, originalStretchRect);
    PatchVtable(d, slot::SetRenderTarget, HookSetRenderTarget, originalSetRenderTarget);
    PatchVtable(d, slot::Clear, HookClear, originalClear);
    PatchVtable(d, slot::DrawPrimitive, HookDrawPrimitive, originalDrawPrimitive);
    PatchVtable(d, slot::DrawIndexedPrimitive, HookDrawIndexedPrimitive, originalDrawIndexedPrimitive);
    PatchVtable(d, slot::DrawPrimitiveUP, HookDrawPrimitiveUP, originalDrawPrimitiveUP);
    PatchVtable(d, slot::DrawIndexedPrimitiveUP, HookDrawIndexedPrimitiveUP, originalDrawIndexedPrimitiveUP);
    PatchVtable(d, slot::CreateVertexShader, HookCreateVertexShader, originalCreateVertexShader);
    PatchVtable(d, slot::SetVertexShader, HookSetVertexShader, originalSetVertexShader);
    PatchVtable(d, slot::SetVertexShaderConstantF, HookSetVertexShaderConstantF, originalSetVertexShaderConstantF);
    PatchVtable(d, slot::CreateQuery, HookCreateQuery, originalCreateQuery);
    return result;
}

} // namespace

void InstallHooks(IDirect3D9* d3d) { PatchVtable(d3d, slot::CreateDevice, HookCreateDevice, originalCreateDevice); }
