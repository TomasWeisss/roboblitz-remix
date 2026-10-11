// The game draws everything with vertex shaders and never calls SetTransform, so Remix has no
// camera. Before each draw we look at the bound vertex shader's constant table, read the camera
// matrix from its constants, and hand Remix fixed-function transforms built from it.
//
// Hooks patch the vtables of the real IDirect3D9 / IDirect3DDevice9 (the Remix bridge client).

#include "hooks.h"

#include "camera.h"
#include "config.h"
#include "light_conversion.h"
#include "log.h"
#include "shader_info.h"
#include "texture_analysis.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <d3d9.h>
#include <d3d9types.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <winerror.h>
#include <winnt.h>

namespace {

// vtable slots, in declaration order of d3d9.h
namespace slot {
    constexpr int CreateDevice = 16;              // IDirect3D9
    constexpr int Reset = 16;                     // IDirect3DDevice9 ...
    constexpr int Present = 17;
    constexpr int CreateTexture = 23;
    constexpr int UpdateTexture = 31;
    constexpr int StretchRect = 34;
    constexpr int SetRenderTarget = 37;
    constexpr int Clear = 43;
    constexpr int SetTexture = 65;
    constexpr int SetSamplerState = 69;
    constexpr int DrawPrimitive = 81;
    constexpr int DrawIndexedPrimitive = 82;
    constexpr int DrawPrimitiveUP = 83;
    constexpr int DrawIndexedPrimitiveUP = 84;
    constexpr int CreateVertexShader = 91;
    constexpr int SetVertexShader = 92;
    constexpr int SetVertexShaderConstantF = 94;
    constexpr int CreatePixelShader = 106;
    constexpr int SetPixelShader = 107;
    constexpr int SetPixelShaderConstantF = 109;
    constexpr int CreateQuery = 118;

    constexpr int QueryIssue = 6;

    constexpr int TextureLockRect = 19;   // IDirect3DTexture9
    constexpr int TextureUnlockRect = 20;
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
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using SetPixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPixelShaderConstantFFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using CreateQueryFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DQUERYTYPE, IDirect3DQuery9**);
using CreateTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                     IDirect3DTexture9**, HANDLE*);
using UpdateTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
using SetTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetSamplerStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using LockRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using UnlockRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT);
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
CreatePixelShaderFn originalCreatePixelShader;
SetPixelShaderFn originalSetPixelShader;
SetPixelShaderConstantFFn originalSetPixelShaderConstantF;
CreateQueryFn originalCreateQuery;
QueryIssueFn originalQueryIssue;
CreateTextureFn originalCreateTexture;
UpdateTextureFn originalUpdateTexture;
SetTextureFn originalSetTexture;
SetSamplerStateFn originalSetSamplerState;
LockRectFn originalLockRect;
UnlockRectFn originalUnlockRect;

constexpr int kKindCount = 6;
constexpr UINT kMaxVertexConstants = 256; // vs_3_0
constexpr UINT kMaxPixelConstants = 224;  // ps_3_0
constexpr DWORD kMaxEnabledLights = 8;    // D3D9 limit on lights enabled at once
constexpr DWORD kMaxSamplers = 16;          // ps_3_0
constexpr DWORD kSamplerStateCount = 14;    // D3DSAMP_ADDRESSU (1) .. D3DSAMP_DMAPOFFSET (13)
constexpr DWORD kCopiedSamplerStates = 11;  // ADDRESSU .. SRGBTEXTURE: what defines how a texture is sampled

struct DrawStats {
    unsigned drawn[kKindCount] = {};
    unsigned skipped[kKindCount] = {};
    unsigned fixedFunction = 0;
    unsigned badCamera = 0;
    unsigned albedoMoved = 0;   // a material texture was moved into stage 0
    unsigned albedoCleared = 0; // stage 0 was emptied (only normal maps or a lightmap)
};

// Content verdicts for textures, filled when the game uploads texel data. Uploads may come
// from another thread than drawing, so this has its own lock.
struct TextureState {
    struct Info {
        TextureContent content = TextureContent::Unknown;
        UINT analyzedWidth = 0; // the largest analysed mip level wins
    };
    struct PendingLock {
        const void* bits;
        INT pitch;
    };

    std::mutex mutex;
    std::unordered_map<IDirect3DBaseTexture9*, Info> info;
    std::map<std::pair<IDirect3DTexture9*, UINT>, PendingLock> locks;
} textures;

TextureContent ContentOf(IDirect3DBaseTexture9* texture) {
    if (!texture) return TextureContent::Unknown;
    std::lock_guard<std::mutex> lock(textures.mutex);
    const auto it = textures.info.find(texture);
    return it != textures.info.end() ? it->second.content : TextureContent::Unknown;
}

// UE3 dynamic lights, read from the light passes and handed to Remix as D3D9 lights
struct LightState {
    std::vector<GameLight> seen;     // lights drawn this frame, deduplicated
    std::vector<D3DLIGHT9> pending;  // last frame's lights, submitted to Remix during this frame
    size_t next = 0;                 // next entry of `pending` to submit
    DWORD enabledSlots = 0;          // light slots currently enabled on the device
    unsigned counts[3] = {};         // last frame: point, spot, directional
};

// The game uses a single device on one thread
struct DeviceState {
    float vsConstants[kMaxVertexConstants][4] = {};
    float psConstants[kMaxPixelConstants][4] = {};
    LightState lights;
    std::unordered_map<IDirect3DVertexShader9*, VertexShaderInfo> shaders;
    const VertexShaderInfo* currentShader = nullptr;
    std::unordered_map<IDirect3DPixelShader9*, PixelShaderInfo> pixelShaders;
    const PixelShaderInfo* currentPixelShader = nullptr;
    int nextShaderId = 1;

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
    UINT backBufferWidth = 0, backBufferHeight = 0;
    bool renderingToBackbuffer = true;
    bool renderTargetScreenSized = true; // Remix only ray traces draws to screen-sized targets

    ShaderKind lastDrawKind = ShaderKind::Screen; // effective kind of the last shader draw

    // What the game has bound, tracked so we can override stage 0 and restore it afterwards
    IDirect3DBaseTexture9* boundTextures[kMaxSamplers] = {};
    DWORD samplerStates[kMaxSamplers][kSamplerStateCount] = {};

    // Stage 0 override for the current draw: the sampler register whose texture is in stage 0
    // (kNoTexture: stage 0 emptied), or kNotOverridden
    int albedoSource = -2;
    unsigned activeOcclusionQueries = 0;

    DrawStats stats;
    unsigned frame = 0;

    // Frame dump: logs every draw of one frame, merging consecutive identical lines
    bool dumping = false;
    std::string dumpLine;
    unsigned dumpRepeat = 0;
    unsigned dumpPrimitives = 0;
    std::set<const VertexShaderInfo*> dumpedVertexShaders;
    std::set<const PixelShaderInfo*> dumpedPixelShaders;
} state;

void RememberBackbuffer(IDirect3DDevice9* device) {
    IDirect3DSurface9* surface = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface)) && surface) {
        state.backBuffer = surface;
        D3DSURFACE_DESC desc = {};
        surface->GetDesc(&desc);
        state.backBufferWidth = desc.Width;
        state.backBufferHeight = desc.Height;
        surface->Release();
    }
    
    state.renderingToBackbuffer = true;
    state.renderTargetScreenSized = true;
}

// --- Albedo: Remix uses the texture in stage 0 as albedo ---

constexpr int kNotOverridden = -2;
constexpr int kNoTexture = -1;

// D3D9 defaults, which Reset also restores
void ResetSamplerTracking() {
    for (DWORD s = 0; s < kMaxSamplers; ++s) {
        state.boundTextures[s] = nullptr;
        DWORD* st = state.samplerStates[s];
        for (DWORD t = 0; t < kSamplerStateCount; ++t) st[t] = 0;
        st[D3DSAMP_ADDRESSU] = st[D3DSAMP_ADDRESSV] = st[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        st[D3DSAMP_MAGFILTER] = st[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        st[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        st[D3DSAMP_MAXANISOTROPY] = 1;
    }
    state.albedoSource = kNotOverridden;
}

// Picks the sampler register holding the albedo: the first material texture (Texture2D_*)
// that is not a normal map. Lightmaps and light attenuation textures are never albedo.
int ChooseAlbedoSampler(const PixelShaderInfo& shader) {
    bool hasMaterialTexture = false;
    for (size_t reg = 0; reg < shader.samplers.size() && reg < kMaxSamplers; ++reg) {
        if (shader.samplers[reg].rfind("Texture2D_", 0) != 0) continue;
        hasMaterialTexture = true;
        if (ContentOf(state.boundTextures[reg]) != TextureContent::NormalMap) return static_cast<int>(reg);
    }
    if (hasMaterialTexture) return kNoTexture; // only normal maps
    const bool samplesSomething = !shader.samplers.empty() && !shader.samplers[0].empty();
    return samplesSomething ? kNoTexture : kNotOverridden; // e.g. a lightmap in s0
}

void OverrideAlbedo(IDirect3DDevice9* device) {
    const PixelShaderInfo* shader = state.currentPixelShader;
    if (!GetConfig().fixAlbedo || !shader) return;

    const int source = ChooseAlbedoSampler(*shader);
    if (source == kNotOverridden || source == 0) return;

    originalSetTexture(device, 0, source >= 0 ? state.boundTextures[source] : nullptr);
    if (source > 0) {
        for (DWORD t = 1; t <= kCopiedSamplerStates; ++t) {
            const DWORD value = state.samplerStates[source][t];
            if (value != state.samplerStates[0][t]) originalSetSamplerState(device, 0, D3DSAMPLERSTATETYPE(t), value);
        }
    }
    state.albedoSource = source;
    ++(source >= 0 ? state.stats.albedoMoved : state.stats.albedoCleared);
}

// Puts back what the game had in stage 0
void RestoreAlbedo(IDirect3DDevice9* device) {
    const int source = state.albedoSource;
    if (source == kNotOverridden) return;

    originalSetTexture(device, 0, state.boundTextures[0]);
    if (source > 0) {
        for (DWORD t = 1; t <= kCopiedSamplerStates; ++t) {
            const DWORD value = state.samplerStates[0][t];
            if (value != state.samplerStates[source][t]) originalSetSamplerState(device, 0, D3DSAMPLERSTATETYPE(t), value);
        }
    }
    state.albedoSource = kNotOverridden;
}

// For the frame dump: which texture Remix gets as albedo
std::string DescribeAlbedo() {
    const PixelShaderInfo* shader = state.currentPixelShader;
    const int source = state.albedoSource == kNotOverridden ? 0 : state.albedoSource;
    if (source == kNoTexture) return "none";
    if (!shader || static_cast<size_t>(source) >= shader->samplers.size() || shader->samplers[source].empty()) {
        return "-";
    }
    return shader->samplers[source] + "(" + ToString(ContentOf(state.boundTextures[source])) + ")";
}

// --- Lights ---

const float* PixelConstant(int reg) {
    return reg >= 0 && reg < static_cast<int>(kMaxPixelConstants) ? state.psConstants[reg] : nullptr;
}

const float* VertexConstant(int reg) {
    return reg >= 0 && reg < static_cast<int>(kMaxVertexConstants) ? state.vsConstants[reg] : nullptr;
}

// Reads the light of the current light pass from the shader constants
void CaptureLight() {
    const VertexShaderInfo* vs = state.currentShader;
    const PixelShaderInfo* ps = state.currentPixelShader;
    if (!GetConfig().translateLights || !vs || !ps) return;

    const float* color = PixelConstant(FindFloatRegister(ps->floatRegisters, "LightColorAndFalloffExponent"));
    if (!color) color = PixelConstant(FindFloatRegister(ps->floatRegisters, "LightColor"));
    const float* position = VertexConstant(FindFloatRegister(vs->floatRegisters, "LightPositionAndInvRadius"));
    const float* direction = VertexConstant(FindFloatRegister(vs->floatRegisters, "LightDirection"));
    const float* spotDirection = PixelConstant(FindFloatRegister(ps->floatRegisters, "SpotDirection"));
    const float* spotAngles = PixelConstant(FindFloatRegister(ps->floatRegisters, "SpotAngles"));
    if (!color) return;

    GameLight light;
    if (position && spotDirection && spotAngles) light = MakeSpotLight(position, color, spotDirection, spotAngles);
    else if (position) light = MakePointLight(position, color);
    else if (direction) light = MakeDirectionalLight(direction, color);
    else return;

    // UE3 draws a light once per object it lights
    for (const GameLight& seen : state.lights.seen) {
        if (SameLight(seen, light)) return;
    }
    state.lights.seen.push_back(light);
}

// End of frame: this frame's lights are submitted during the next frame
void FinishLightFrame() {
    LightState& lights = state.lights;
    lights.pending.clear();
    for (unsigned& count : lights.counts) count = 0;
    for (const GameLight& light : lights.seen) {
        D3DLIGHT9 converted;
        if (!ToD3DLight(light, converted)) continue;
        lights.pending.push_back(converted);
        ++lights.counts[static_cast<int>(light.type)];
    }
    lights.seen.clear();
    lights.next = 0;
}

// D3D9 allows 8 enabled lights, but Remix keeps every light it sees enabled during a ray traced
// draw. So each ray traced draw gets the next batch of up to 8 until all lights are submitted.
// Re-submitting every frame keeps Remix from expiring them.
void SubmitLights(IDirect3DDevice9* device) {
    LightState& lights = state.lights;
    if (!GetConfig().translateLights || !state.renderTargetScreenSized) return;

    DWORD count = 0;
    if (lights.next < lights.pending.size()) {
        count = static_cast<DWORD>(std::min<size_t>(kMaxEnabledLights, lights.pending.size() - lights.next));
        for (DWORD i = 0; i < count; ++i) {
            device->SetLight(i, &lights.pending[lights.next + i]);
            device->LightEnable(i, TRUE);
        }
        lights.next += count;
    } else if (!lights.pending.empty() || lights.enabledSlots == 0) {
        return; // all submitted for this frame; leave the last batch enabled
    }
    for (DWORD i = count; i < lights.enabledSlots; ++i) device->LightEnable(i, FALSE);
    lights.enabledSlots = count;
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
        case ShaderKind::Light:
        case ShaderKind::Depth:
            if (effectiveKind == ShaderKind::Light) CaptureLight();
            if (effectiveKind == ShaderKind::Light ? config.skipLightPasses : config.skipDepthPasses) {
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
            if (effectiveKind == ShaderKind::World) {
                OverrideAlbedo(device);
                SubmitLights(device);
            }
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

    // Everything else is screen-space: give it identity transforms instead of the camera
    if (draw && !UsesCamera(effectiveKind))
        SetTransforms(device, Identity(), Identity(), Identity());

    (draw ? state.stats.drawn : state.stats.skipped)[static_cast<int>(effectiveKind)]++;
    state.lastDrawKind = effectiveKind;
    return draw;
}

// Predicts what Remix does with a draw, following D3D9Rtx::makeDrawCallType in dxvk-remix
const char* PredictRemix(IDirect3DDevice9* device, bool draw) {
    if (!draw) return "-";
    if (!state.currentShader || state.lastDrawKind != ShaderKind::World) return "raster";

    DWORD colorWrite = 0, alphaBlend = 0;
    device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
    device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
    const DWORD rgb = D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE;
    if ((colorWrite & rgb) != rgb) return "no-color";
    if (alphaBlend) return "RAYTRACED-translucent"; // rtx.enableAlphaBlend defaults to true

    IDirect3DSurface9* target = nullptr;
    D3DSURFACE_DESC desc = {};
    if (SUCCEEDED(device->GetRenderTarget(0, &target)) && target) {
        target->GetDesc(&desc);
        target->Release();
    }
    if (desc.Width != state.backBufferWidth || desc.Height != state.backBufferHeight) return "raster";
    return "RAYTRACED";
}

// PrepareDraw plus frame-dump logging
bool BeforeDraw(IDirect3DDevice9* device, unsigned primitives) {
    const bool draw = PrepareDraw(device);
    if (state.dumping) {
        const VertexShaderInfo* shader = state.currentShader;
        const PixelShaderInfo* pixelShader = state.currentPixelShader;
        if (shader) state.dumpedVertexShaders.insert(shader);
        if (pixelShader) state.dumpedPixelShaders.insert(pixelShader);

        char text[192];
        snprintf(text, sizeof(text), "%s %s VS#%d PS#%d albedo=%s remix=%s -> ", draw ? "draw" : "SKIP",
                 shader ? ToString(state.lastDrawKind) : "fixed-function", shader ? shader->id : 0,
                 pixelShader ? pixelShader->id : 0, DescribeAlbedo().c_str(), PredictRemix(device, draw));
        DumpEvent(text + DescribeRenderTarget(device), primitives);
    }
    return draw;
}

void LogFrameStats() {
    const DrawStats& s = state.stats;
    unsigned normalMaps = 0, colorTextures = 0;
    {
        std::lock_guard<std::mutex> lock(textures.mutex);
        for (const auto& entry : textures.info) {
            ++(entry.second.content == TextureContent::NormalMap ? normalMaps : colorTextures);
        }
    }
    Log("Frame %u albedo: moved %u, cleared %u; textures analysed: %u normal maps, %u other", state.frame,
        s.albedoMoved, s.albedoCleared, normalMaps, colorTextures);
    const unsigned* lightCounts = state.lights.counts;
    Log("Frame %u lights: %u point, %u spot, %u directional", state.frame, lightCounts[0], lightCounts[1],
        lightCounts[2]);
    Log("Frame %u draws: World %u, UI %u, Shadow %u (skipped %u), Screen %u (skipped %u), Depth %u (skipped %u), "
        "Light %u (skipped %u), fixed-function %u, bad camera %u",
        state.frame, s.drawn[0], s.drawn[1], s.drawn[2], s.skipped[2], s.drawn[3], s.skipped[3], s.drawn[4],
        s.skipped[4], s.drawn[5], s.skipped[5], s.fixedFunction, s.badCamera);
}

// --- Device hooks ---

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    state.transformsKnown = false; // Reset restores default device state
    auto reset = originalReset(device, params);

    // Reset invalidates swapchain
    if (SUCCEEDED(reset)) {
        RememberBackbuffer(device);
        ResetSamplerTracking();
    }
    
    return reset;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window,
                                      const RGNDATA* dirty) {
    ++state.frame;
    FinishLightFrame();
    const unsigned f = state.frame;
    if (f == 1 || f == 10 || f == 100 || f == 1000 || f % 5000 == 0) LogFrameStats();
    state.stats = {};

    if (state.dumping) {
        FlushDumpLine();
        Log("Shaders in this dump:");
        for (const VertexShaderInfo* shader : state.dumpedVertexShaders) {
            std::string names;
            for (const std::string& name : shader->constants) names += " " + name;
            Log("  VS#%d %s:%s", shader->id, ToString(shader->kind), names.c_str());
        }
        for (const PixelShaderInfo* shader : state.dumpedPixelShaders) {
            std::string samplers;
            for (size_t i = 0; i < shader->samplers.size(); ++i) {
                if (!shader->samplers[i].empty()) samplers += " s" + std::to_string(i) + "=" + shader->samplers[i];
            }
            Log("  PS#%d samplers:%s", shader->id, samplers.c_str());
        }
        Log("Lights for the next frame:");
        for (const D3DLIGHT9& light : state.lights.pending) {
            Log("  type %d pos (%.0f %.0f %.0f) dir (%.2f %.2f %.2f) color (%.2f %.2f %.2f) range %.0f cone %.0f/%.0f deg",
                light.Type, light.Position.x, light.Position.y, light.Position.z, light.Direction.x, light.Direction.y,
                light.Direction.z, light.Diffuse.r, light.Diffuse.g, light.Diffuse.b, light.Range,
                light.Theta * 57.2958f, light.Phi * 57.2958f);
        }
        Log("End of frame dump");
        state.dumping = false;
        state.dumpedVertexShaders.clear();
        state.dumpedPixelShaders.clear();
    }
    // Dump the next frame at the configured frame number, or on Ctrl+F11
    const bool hotkey = (GetAsyncKeyState(kDumpKey) & 1) && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
    if (hotkey || f == GetConfig().dumpFrame) {
        Log("Frame dump of frame %u (draw/SKIP, kind, shaders, albedo given to Remix, predicted Remix handling, render target):", f + 1);
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
    if (targetIndex == 0) {
        state.renderingToBackbuffer = target == state.backBuffer;
        D3DSURFACE_DESC desc = {};
        if (target) target->GetDesc(&desc);
        state.renderTargetScreenSized = desc.Width == state.backBufferWidth && desc.Height == state.backBufferHeight;
    }
    
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
    if (!BeforeDraw(device, count)) return D3D_OK;
    const HRESULT result = originalDrawPrimitive(device, type, start, count);
    RestoreAlbedo(device);
    return result;
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT baseVertex,
                                                   UINT minIndex, UINT numVertices, UINT startIndex, UINT count) {
    if (!BeforeDraw(device, count)) return D3D_OK;
    const HRESULT result =
        originalDrawIndexedPrimitive(device, type, baseVertex, minIndex, numVertices, startIndex, count);
    RestoreAlbedo(device);
    return result;
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count,
                                              const void* data, UINT stride) {
    if (!BeforeDraw(device, count)) return D3D_OK;
    const HRESULT result = originalDrawPrimitiveUP(device, type, count, data, stride);
    RestoreAlbedo(device);
    return result;
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitiveUP(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT minIndex,
                                                     UINT numVertices, UINT count, const void* indices,
                                                     D3DFORMAT indexFormat, const void* vertices, UINT stride) {
    if (!BeforeDraw(device, count)) return D3D_OK;
    const HRESULT result = originalDrawIndexedPrimitiveUP(device, type, minIndex, numVertices, count, indices,
                                                          indexFormat, vertices, stride);
    RestoreAlbedo(device);
    return result;
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
    info.id = state.nextShaderId++;
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

HRESULT STDMETHODCALLTYPE HookCreatePixelShader(IDirect3DDevice9* device, const DWORD* function,
                                                IDirect3DPixelShader9** shader) {
    const HRESULT result = originalCreatePixelShader(device, function, shader);
    if (FAILED(result) || !shader || !*shader) return result;

    PixelShaderInfo info;
    ParsePixelShader(function, info);
    info.id = state.nextShaderId++;
    state.pixelShaders[*shader] = std::move(info);
    return result;
}

HRESULT STDMETHODCALLTYPE HookSetPixelShader(IDirect3DDevice9* device, IDirect3DPixelShader9* shader) {
    const auto it = shader ? state.pixelShaders.find(shader) : state.pixelShaders.end();
    state.currentPixelShader = it != state.pixelShaders.end() ? &it->second : nullptr;
    return originalSetPixelShader(device, shader);
}

HRESULT STDMETHODCALLTYPE HookSetPixelShaderConstantF(IDirect3DDevice9* device, UINT start, const float* data,
                                                      UINT count) {
    if (data && start < kMaxPixelConstants) {
        const UINT copied = count < kMaxPixelConstants - start ? count : kMaxPixelConstants - start;
        memcpy(state.psConstants[start], data, copied * sizeof(float[4]));
    }
    return originalSetPixelShaderConstantF(device, start, data, count);
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



// --- Texture tracking ---

HRESULT STDMETHODCALLTYPE HookSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture) {
    if (stage < kMaxSamplers) state.boundTextures[stage] = texture;
    return originalSetTexture(device, stage, texture);
}

HRESULT STDMETHODCALLTYPE HookSetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type,
                                              DWORD value) {
    if (sampler < kMaxSamplers && DWORD(type) < kSamplerStateCount) state.samplerStates[sampler][type] = value;
    return originalSetSamplerState(device, sampler, type, value);
}

// Remember where the texel data of a whole mip level is, so UnlockRect can analyse it
HRESULT STDMETHODCALLTYPE HookLockRect(IDirect3DTexture9* texture, UINT level, D3DLOCKED_RECT* locked,
                                       const RECT* rect, DWORD flags) {
    const HRESULT result = originalLockRect(texture, level, locked, rect, flags);
    if (SUCCEEDED(result) && locked && !rect && !(flags & D3DLOCK_READONLY)) {
        std::lock_guard<std::mutex> lock(textures.mutex);
        textures.locks[{texture, level}] = {locked->pBits, locked->Pitch};
    }
    return result;
}

// The game has written the texels: classify them before the data goes away
HRESULT STDMETHODCALLTYPE HookUnlockRect(IDirect3DTexture9* texture, UINT level) {
    TextureState::PendingLock pending = {};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(textures.mutex);
        const auto it = textures.locks.find({texture, level});
        if (it != textures.locks.end()) {
            pending = it->second;
            found = true;
            textures.locks.erase(it);
        }
    }
    D3DSURFACE_DESC desc = {};
    if (found && SUCCEEDED(texture->GetLevelDesc(level, &desc))) {
        const TextureContent content = ClassifyTexture(desc.Format, desc.Width, desc.Height, pending.bits, pending.pitch);
        if (content != TextureContent::Unknown) {
            std::lock_guard<std::mutex> lock(textures.mutex);
            TextureState::Info& info = textures.info[texture];
            if (desc.Width >= info.analyzedWidth) info = {content, desc.Width};
        }
    }
    return originalUnlockRect(texture, level);
}

HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels,
                                            DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** texture,
                                            HANDLE* shared) {
    const HRESULT result = originalCreateTexture(device, width, height, levels, usage, format, pool, texture, shared);
    if (SUCCEEDED(result) && texture && *texture) {
        {
            // A new texture may reuse a released one's address
            std::lock_guard<std::mutex> lock(textures.mutex);
            textures.info.erase(*texture);
        }
        PatchVtable(*texture, slot::TextureLockRect, HookLockRect, originalLockRect);
        PatchVtable(*texture, slot::TextureUnlockRect, HookUnlockRect, originalUnlockRect);
    }
    return result;
}

// Uploads through a system-memory staging texture: the destination gets the source's verdict
HRESULT STDMETHODCALLTYPE HookUpdateTexture(IDirect3DDevice9* device, IDirect3DBaseTexture9* source,
                                            IDirect3DBaseTexture9* dest) {
    {
        std::lock_guard<std::mutex> lock(textures.mutex);
        const auto it = textures.info.find(source);
        if (it != textures.info.end()) textures.info[dest] = it->second;
    }
    return originalUpdateTexture(device, source, dest);
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
    {
        std::lock_guard<std::mutex> lock(textures.mutex);
        textures.info.clear();
        textures.locks.clear();
    }
    IDirect3DDevice9* d = *device;
    RememberBackbuffer(d);
    ResetSamplerTracking();
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
    PatchVtable(d, slot::CreatePixelShader, HookCreatePixelShader, originalCreatePixelShader);
    PatchVtable(d, slot::SetPixelShader, HookSetPixelShader, originalSetPixelShader);
    PatchVtable(d, slot::SetPixelShaderConstantF, HookSetPixelShaderConstantF, originalSetPixelShaderConstantF);
    PatchVtable(d, slot::CreateQuery, HookCreateQuery, originalCreateQuery);
    PatchVtable(d, slot::CreateTexture, HookCreateTexture, originalCreateTexture);
    PatchVtable(d, slot::UpdateTexture, HookUpdateTexture, originalUpdateTexture);
    PatchVtable(d, slot::SetTexture, HookSetTexture, originalSetTexture);
    PatchVtable(d, slot::SetSamplerState, HookSetSamplerState, originalSetSamplerState);
    return result;
}

} // namespace

void InstallHooks(IDirect3D9* d3d) { PatchVtable(d3d, slot::CreateDevice, HookCreateDevice, originalCreateDevice); }
