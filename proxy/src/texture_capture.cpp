#include "texture_capture.h"

#include "dds.h"
#include "vtable.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <unordered_map>

namespace {

namespace slot {
    constexpr int CreateTexture = 23; // IDirect3DDevice9
    constexpr int UpdateTexture = 31;
    constexpr int LockRect = 19;      // IDirect3DTexture9
    constexpr int UnlockRect = 20;
    constexpr int SurfaceLockRect = 13; // IDirect3DSurface9
    constexpr int SurfaceUnlockRect = 14;
}

using CreateTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                     IDirect3DTexture9**, HANDLE*);
using LockRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using UnlockRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT);
using UpdateTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DBaseTexture9*, IDirect3DBaseTexture9*);
using SurfaceLockRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD);
using SurfaceUnlockRectFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*);

CreateTextureFn originalCreateTexture;
LockRectFn originalLockRect;
UnlockRectFn originalUnlockRect;
UpdateTextureFn originalUpdateTexture;
SurfaceLockRectFn originalSurfaceLockRect;
SurfaceUnlockRectFn originalSurfaceUnlockRect;

// A write lock of a top level that covers all of it
struct OpenLock {
    const uint8_t* bits;
    INT pitch;
    IDirect3DTexture9* texture = nullptr; // for locks through the level's surface
};

std::mutex mutex;
std::unordered_map<IDirect3DBaseTexture9*, std::shared_ptr<const CapturedTexture>> captured;
std::unordered_map<IDirect3DTexture9*, OpenLock> openLocks;
std::unordered_map<IDirect3DSurface9*, OpenLock> openSurfaceLocks;
std::unordered_map<IDirect3DBaseTexture9*, std::string> lockHistory; // for textures not captured

constexpr UINT kSurfaceLevel = 99; // in lock notes: a lock through a surface other than the top level

// Remembers a lock that was not captured, for DescribeTextureLocks
void NoteLock(IDirect3DTexture9* texture, UINT level, const RECT* rect, DWORD flags, HRESULT result, INT pitch) {
    std::string& history = lockHistory[texture];
    if (history.size() > 400) return;
    char text[96];
    snprintf(text, sizeof(text), "%sL%u f%lx%s hr%lx p%d", history.empty() ? "" : "; ", level, flags,
             rect ? " rect" : "", static_cast<unsigned long>(result), pitch);
    history += text;
}

bool CoversLevel(IDirect3DTexture9* texture, const RECT* rect, D3DSURFACE_DESC& desc) {
    if (FAILED(texture->GetLevelDesc(0, &desc))) return false;
    return !rect || (rect->left <= 0 && rect->top <= 0 && rect->right >= LONG(desc.Width) &&
                     rect->bottom >= LONG(desc.Height));
}

HRESULT STDMETHODCALLTYPE HookLockRect(IDirect3DTexture9* texture, UINT level, D3DLOCKED_RECT* locked,
                                       const RECT* rect, DWORD flags) {
    const HRESULT result = originalLockRect(texture, level, locked, rect, flags);
    D3DSURFACE_DESC desc;
    std::lock_guard<std::mutex> lock(mutex);
    if (SUCCEEDED(result) && level == 0 && locked && locked->pBits && !(flags & D3DLOCK_READONLY) &&
        CoversLevel(texture, rect, desc)) {
        openLocks[texture] = {static_cast<const uint8_t*>(locked->pBits), locked->Pitch};
    } else {
        NoteLock(texture, level, rect, flags, result, locked ? locked->Pitch : 0);
    }
    return result;
}

// Copies the locked top level of `texture`; called with the mutex held, before the unlock
void CaptureTopLevel(IDirect3DTexture9* texture, const OpenLock& open) {
    D3DSURFACE_DESC desc;
    LevelLayout layout;
    if (FAILED(texture->GetLevelDesc(0, &desc)) || !GetLevelLayout(desc.Format, desc.Width, desc.Height, layout) ||
        size_t(open.pitch) < layout.rowBytes) {
        NoteLock(texture, 0, nullptr, 0, E_FAIL, open.pitch);
        return;
    }
    auto copy = std::make_shared<CapturedTexture>();
    copy->format = desc.Format;
    copy->width = desc.Width;
    copy->height = desc.Height;
    copy->level0.resize(layout.Size());
    for (size_t row = 0; row < layout.rows; ++row) {
        memcpy(&copy->level0[row * layout.rowBytes], open.bits + row * open.pitch, layout.rowBytes);
    }
    captured[texture] = std::move(copy);
}

HRESULT STDMETHODCALLTYPE HookUnlockRect(IDirect3DTexture9* texture, UINT level) {
    if (level == 0) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto open = openLocks.find(texture);
        if (open != openLocks.end()) {
            CaptureTopLevel(texture, open->second);
            openLocks.erase(open);
        }
    }
    return originalUnlockRect(texture, level);
}

// The game can also write a level through its surface (GetSurfaceLevel, D3DXLoadSurfaceFromMemory)
HRESULT STDMETHODCALLTYPE HookSurfaceLockRect(IDirect3DSurface9* surface, D3DLOCKED_RECT* locked, const RECT* rect,
                                              DWORD flags) {
    const HRESULT result = originalSurfaceLockRect(surface, locked, rect, flags);
    if (FAILED(result) || !locked || !locked->pBits || (flags & D3DLOCK_READONLY)) return result;

    IDirect3DTexture9* texture = nullptr;
    if (FAILED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&texture))) || !texture) {
        return result; // not a texture level: render target, back buffer, ...
    }
    std::lock_guard<std::mutex> lock(mutex);
    // A texture-level lock may be implemented through here; that one is already handled
    if (!openLocks.count(texture)) {
        D3DSURFACE_DESC surfaceDesc, levelDesc;
        const bool topLevel = SUCCEEDED(surface->GetDesc(&surfaceDesc)) && CoversLevel(texture, rect, levelDesc) &&
                              surfaceDesc.Width == levelDesc.Width && surfaceDesc.Height == levelDesc.Height;
        if (topLevel) openSurfaceLocks[surface] = {static_cast<const uint8_t*>(locked->pBits), locked->Pitch, texture};
        else NoteLock(texture, kSurfaceLevel, rect, flags, result, locked->Pitch);
    }
    texture->Release(); // GetContainer added a reference; the surface keeps the texture alive
    return result;
}

HRESULT STDMETHODCALLTYPE HookSurfaceUnlockRect(IDirect3DSurface9* surface) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto open = openSurfaceLocks.find(surface);
        if (open != openSurfaceLocks.end()) {
            CaptureTopLevel(open->second.texture, open->second);
            openSurfaceLocks.erase(open);
        }
    }
    return originalSurfaceUnlockRect(surface);
}

HRESULT STDMETHODCALLTYPE HookCreateTexture(IDirect3DDevice9* device, UINT width, UINT height, UINT levels,
                                            DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** texture,
                                            HANDLE* sharedHandle) {
    const HRESULT result =
        originalCreateTexture(device, width, height, levels, usage, format, pool, texture, sharedHandle);
    if (SUCCEEDED(result) && texture && *texture) {
        {
            // A released texture's address can be reused by a new one
            std::lock_guard<std::mutex> lock(mutex);
            captured.erase(*texture);
            openLocks.erase(*texture);
            lockHistory.erase(*texture);
        }
        PatchVtable(*texture, slot::LockRect, HookLockRect, originalLockRect);
        PatchVtable(*texture, slot::UnlockRect, HookUnlockRect, originalUnlockRect);
        static bool surfacesHooked = false;
        IDirect3DSurface9* surface = nullptr;
        if (!surfacesHooked && SUCCEEDED((*texture)->GetSurfaceLevel(0, &surface)) && surface) {
            PatchVtable(surface, slot::SurfaceLockRect, HookSurfaceLockRect, originalSurfaceLockRect);
            PatchVtable(surface, slot::SurfaceUnlockRect, HookSurfaceUnlockRect, originalSurfaceUnlockRect);
            surface->Release();
            surfacesHooked = true;
        }
    }
    return result;
}

// A texture filled from a system memory one (D3DPOOL_DEFAULT textures cannot be locked) gets its
// contents, if their top levels have the same size
HRESULT STDMETHODCALLTYPE HookUpdateTexture(IDirect3DDevice9* device, IDirect3DBaseTexture9* source,
                                            IDirect3DBaseTexture9* destination) {
    const HRESULT result = originalUpdateTexture(device, source, destination);
    if (SUCCEEDED(result) && source && destination && destination->GetType() == D3DRTYPE_TEXTURE) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = captured.find(source);
        D3DSURFACE_DESC desc;
        if (it != captured.end() &&
            SUCCEEDED(static_cast<IDirect3DTexture9*>(destination)->GetLevelDesc(0, &desc)) &&
            desc.Width == it->second->width && desc.Height == it->second->height && desc.Format == it->second->format) {
            captured[destination] = it->second;
        }
    }
    return result;
}

} // namespace

void InstallTextureCapture(IDirect3DDevice9* device) {
    PatchVtable(device, slot::CreateTexture, HookCreateTexture, originalCreateTexture);
    PatchVtable(device, slot::UpdateTexture, HookUpdateTexture, originalUpdateTexture);
}

std::string DescribeTextureLocks(IDirect3DBaseTexture9* texture) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = lockHistory.find(texture);
    return it != lockHistory.end() ? it->second : "never locked";
}

std::shared_ptr<const CapturedTexture> FindCapturedTexture(IDirect3DBaseTexture9* texture) {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = captured.find(texture);
    return it != captured.end() ? it->second : nullptr;
}
