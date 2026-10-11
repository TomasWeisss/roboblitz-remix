#pragma once

// Appends a printf-style line to roboblitz_remix_proxy.log next to the proxy DLL.
// The file is truncated when the first line of a process is written.
void Log(const char* format, ...);
