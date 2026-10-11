#include "log.h"

#include "config.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

void Log(const char* format, ...) {
    static std::mutex mutex;
    static FILE* file = _wfopen((ModuleDirectory() + L"roboblitz_remix_proxy.log").c_str(), L"w");
    if (!file) return;

    std::lock_guard<std::mutex> lock(mutex);
    SYSTEMTIME time;
    GetLocalTime(&time);
    fprintf(file, "[%02d:%02d:%02d.%03d] ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fputc('\n', file);
    fflush(file);
}
