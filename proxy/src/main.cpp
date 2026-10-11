// d3d9.dll proxy: loads the real d3d9 (the Remix bridge, renamed) and hooks the device.

#include "config.h"
#include "hooks.h"
#include "log.h"

#include <d3d9.h>

namespace {

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT);

Direct3DCreate9Fn LoadRealDirect3DCreate9() {
    const Config& config = GetConfig();
    std::wstring path = ModuleDirectory() + config.remixDll;
    HMODULE module = LoadLibraryW(path.c_str());

    if (!module) {
        // Fall back to the system d3d9 so the game still runs without Remix
        Log("Could not load %ls (error %lu); falling back to the system d3d9.dll", path.c_str(), GetLastError());
        wchar_t system[MAX_PATH];
        GetSystemDirectoryW(system, MAX_PATH);
        path = std::wstring(system) + L"\\d3d9.dll";
        module = LoadLibraryW(path.c_str());
    }
    if (!module) {
        Log("Could not load %ls (error %lu)", path.c_str(), GetLastError());
        return nullptr;
    }

    Log("Loaded %ls", path.c_str());
    Log("Config: SkipScreenPasses=%d SkipShadowPasses=%d SkipDepthPasses=%d SkipLightPasses=%d FixAlbedo=%d TranslateLights=%d "
        "UseLocalToWorld=%d "
        "LogShaders=%d DumpFrame=%u",
        config.skipScreenPasses, config.skipShadowPasses, config.skipDepthPasses, config.skipLightPasses, config.fixAlbedo, config.translateLights,
        config.useLocalToWorld, config.logShaders, config.dumpFrame);
    return reinterpret_cast<Direct3DCreate9Fn>(GetProcAddress(module, "Direct3DCreate9"));
}

} // namespace

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion) {
    static const Direct3DCreate9Fn real = LoadRealDirect3DCreate9();
    if (!real) return nullptr;

    IDirect3D9* d3d = real(sdkVersion);
    if (d3d) InstallHooks(d3d);
    return d3d;
}
