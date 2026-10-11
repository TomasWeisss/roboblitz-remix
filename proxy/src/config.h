#pragma once

#include <string>

// Settings from roboblitz_remix_proxy.ini next to the proxy DLL. Missing keys use these defaults.
struct Config {
    std::wstring remixDll = L"d3d9_remix.dll"; // the real d3d9 (Remix bridge), relative to the proxy
    bool skipScreenPasses = true;              // full-screen passes (post-process, gamma, fog)
    bool skipShadowPasses = true;              // shadow-map depth rendering
    bool skipDepthPasses = true;                // depth prepass (UE3 draws it with colour writes on)
    bool translateLights = true;                // hand the game's dynamic lights to Remix as D3D9 lights
    bool fixAlbedo = true;                      // move the material's colour texture into stage 0 for Remix
    bool skipLightPasses = true;                // per-light additive passes; Remix does its own lighting
    bool useLocalToWorld = false;              // pass LocalToWorld as WORLD instead of identity
    bool logShaders = false;                   // log every vertex shader's constants on creation
    unsigned dumpFrame = 0;                    // log every draw of the frame after this one (0 = off)
};

// Directory of the proxy DLL, with trailing backslash
const std::wstring& ModuleDirectory();

const Config& GetConfig();
