# RoboBlitz RTX

RTX Remix support for *RoboBlitz* (2006, early UE3, D3D9, 32-bit). Tested with Remix runtime
**remix-1.5.2+68edea01**.

UE3 only uses vertex shaders and never calls `SetTransform`, so Remix sees no camera. A `d3d9.dll`
proxy (`proxy/`) loads Remix (renamed `d3d9_remix.dll`) and, per draw, classifies the vertex shader
by its constants, decomposes `ViewProjectionMatrix` into `SetTransform` calls, skips passes Remix
replaces (screen, shadow, depth, per-light), turns UE3 light passes into D3D9 lights, and moves the
albedo texture into stage 0.

## Layout

- `proxy/src/` — `hooks.cpp` (draw logic), `shader_info` (shader classification), `camera`,
  `light_conversion`, `texture_analysis` (normal-map detection), `config` (ini); tests in `proxy/tests/`
- `game/` — files copied into the game dir (`rtx.conf`, `roboblitz_remix_proxy.ini` with documented toggles)
- `shaders/patches/` — patches to the game's `.usf` shaders (originals not committed)
- `scripts/` — `build.ps1`, `deploy.ps1`, `run.ps1`

## Usage

Needs VS 2022 (C++), CMake, the Remix runtime, and `local.config.psd1` (copy from the `.example`).

```powershell
.\scripts\build.ps1     # build (Win32) + run tests
.\scripts\deploy.ps1    # build, install proxy + Remix + game\ files + shader patches
                        # (-SkipRuntime, -NoProxy, -NoShaderPatches)
.\scripts\run.ps1       # launch via Steam; -Direct starts RoboGame.exe (needs Steam.dll, handled)
```

Log: `Binaries/roboblitz_remix_proxy.log`. Ctrl+F11 in game dumps every draw of the next frame.
Changing shaders deletes `LocalShaderCache.upk`, so the next start recompiles (slow once).
