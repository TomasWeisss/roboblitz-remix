#pragma once

#include <d3d9.h>

// Hooks IDirect3D9::CreateDevice so every device the game creates gets the Remix fixes.
void InstallHooks(IDirect3D9* d3d);
