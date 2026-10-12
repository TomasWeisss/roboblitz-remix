#pragma once

#include <windows.h>

// Points slot `index` of a COM object's vtable at `hook`, storing the previous target in `original`
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
