#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

// Albedo expressions of material pixel shaders, keyed by HashShaderBytecode. The compiler hook
// adds them as the game compiles shaders; the file keeps them for shaders the game later loads
// from its shader cache. One line per shader: hash (16 hex digits), source, expression,
// separated by tabs.
class MaterialMap {
public:
    struct Entry {
        std::string source;     // the material function the expression comes from: diffuse, emissive, opacity, mask
        std::string expression; // HLSL
    };

    // Reads the file and appends to it from then on. Returns the number of entries read.
    size_t Load(const std::wstring& path);

    bool Find(uint64_t hash, Entry& out) const;

    // Remembers and appends an entry. A hash already known keeps its first entry; returns false then.
    bool Add(uint64_t hash, const Entry& entry);

    size_t Size() const;

private:
    mutable std::mutex mutex_;
    std::wstring path_;
    std::unordered_map<uint64_t, Entry> entries_;
};
