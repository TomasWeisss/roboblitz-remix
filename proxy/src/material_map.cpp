#include "material_map.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

size_t MaterialMap::Load(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;
    entries_.clear();

    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        const size_t sourceEnd = line.find('\t', 17);
        if (line.size() < 18 || line[16] != '\t' || sourceEnd == std::string::npos) continue;
        char* end = nullptr;
        const uint64_t hash = std::strtoull(line.c_str(), &end, 16);
        if (end != line.c_str() + 16) continue;
        entries_.emplace(hash, Entry{line.substr(17, sourceEnd - 17), line.substr(sourceEnd + 1)});
    }
    return entries_.size();
}

bool MaterialMap::Find(uint64_t hash, Entry& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(hash);
    if (it == entries_.end()) return false;
    out = it->second;
    return true;
}

bool MaterialMap::Add(uint64_t hash, const Entry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!entries_.emplace(hash, entry).second) return false;
    if (!path_.empty()) {
        std::ofstream file(path_, std::ios::app);
        char key[17];
        snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash));
        file << key << '\t' << entry.source << '\t' << entry.expression << '\n';
    }
    return true;
}

size_t MaterialMap::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}
