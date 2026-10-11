#include "config.h"

#include <windows.h>

namespace {

Config Load() {
    Config config;
    const std::wstring path = ModuleDirectory() + L"roboblitz_remix_proxy.ini";
    const wchar_t* section = L"Proxy";

    wchar_t buffer[MAX_PATH];
    GetPrivateProfileStringW(section, L"RemixDll", config.remixDll.c_str(), buffer, MAX_PATH, path.c_str());
    config.remixDll = buffer;

    auto flag = [&](const wchar_t* key, bool fallback) {
        return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, path.c_str()) != 0;
    };
    config.skipScreenPasses = flag(L"SkipScreenPasses", config.skipScreenPasses);
    config.skipShadowPasses = flag(L"SkipShadowPasses", config.skipShadowPasses);
    config.skipDepthPasses = flag(L"SkipDepthPasses", config.skipDepthPasses);
    config.skipLightPasses = flag(L"SkipLightPasses", config.skipLightPasses);
    config.useLocalToWorld = flag(L"UseLocalToWorld", config.useLocalToWorld);
    config.logShaders = flag(L"LogShaders", config.logShaders);
    config.dumpFrame = GetPrivateProfileIntW(section, L"DumpFrame", config.dumpFrame, path.c_str());
    return config;
}

} // namespace

const std::wstring& ModuleDirectory() {
    static const std::wstring directory = [] {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModuleDirectory), &self);
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
        std::wstring result(path, length);
        return result.substr(0, result.find_last_of(L"\\/") + 1);
    }();
    return directory;
}

const Config& GetConfig() {
    static const Config config = Load();
    return config;
}
