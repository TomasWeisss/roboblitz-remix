#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Material code of material pixel shaders, keyed by HashShaderBytecode. The compiler hook adds
// them as the game compiles shaders; the file keeps them for shaders the game later loads from its
// shader cache. The file starts with kHeader; then one line per shader: hash (16 hex digits),
// then name=value properties, separated by tabs.
class MaterialMap {
public:
    // First line of the file. A file without it is from an older proxy and is started afresh.
    static constexpr const char* kHeader = "# roboblitz-remix material map v2";

    struct Entry {
        // The property holding the albedo expression: diffuse, emissive, opacity or mask
        std::string albedo;
        // Material functions (normal, emissive, diffuse, specular, specularpower, opacity, mask,
        // customlighting) as HLSL expressions, and blend, lighting, twosided
        std::vector<std::pair<std::string, std::string>> properties;

        // The value of a property; empty if absent
        const std::string& Get(const std::string& name) const;
        const std::string& AlbedoExpression() const { return Get(albedo); }
    };

    // Reads the file and appends to it from then on. Returns the number of entries read; a file
    // from an older proxy reads as empty and is rewritten (see Outdated).
    size_t Load(const std::wstring& path);

    // The file was from an older proxy: shaders loaded from the game's shader cache are unknown
    // until the cache is rebuilt
    bool Outdated() const { return outdated_; }

    bool Find(uint64_t hash, Entry& out) const;

    // Remembers and appends an entry. A hash already known keeps its first entry; returns false then.
    bool Add(uint64_t hash, const Entry& entry);

    size_t Size() const;

private:
    mutable std::mutex mutex_;
    std::wstring path_;
    bool outdated_ = false;
    std::unordered_map<uint64_t, Entry> entries_;
};
