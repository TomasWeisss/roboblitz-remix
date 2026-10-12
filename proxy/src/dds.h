#pragma once

#include <windows.h>

#include <d3d9types.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Size of one mip level as stored in memory without row padding: block rows for compressed formats
struct LevelLayout {
    size_t rowBytes = 0;
    size_t rows = 0;

    size_t Size() const { return rowBytes * rows; }
};

// Layout of a width x height level of `format`; false for formats without a known size
bool GetLevelLayout(D3DFORMAT format, UINT width, UINT height, LevelLayout& out);

// Short name, e.g. "DXT1", "A8R8G8B8"; the number for formats not listed
const char* FormatName(D3DFORMAT format);

// A DDS file of one mip level. `data` is tightly packed (see GetLevelLayout). Empty if the format
// has no known size.
std::vector<uint8_t> MakeDds(D3DFORMAT format, UINT width, UINT height, const uint8_t* data, size_t size);
