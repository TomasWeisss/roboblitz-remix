#include "material_map.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

const std::string& MaterialMap::Entry::Get(const std::string& name) const {
    static const std::string empty;
    for (const auto& [key, value] : properties) {
        if (key == name) return value;
    }
    return empty;
}

size_t MaterialMap::Load(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;
    outdated_ = false;
    entries_.clear();

    std::ifstream file(path);
    std::string line;
    if (!file || !std::getline(file, line) || line != kHeader) {
        file.close();
        outdated_ = !line.empty();
        std::ofstream(path, std::ios::trunc) << kHeader << '\n';
        return 0;
    }

    while (std::getline(file, line)) {
        if (line.size() < 17 || line[16] != '\t') continue;
        char* end = nullptr;
        const uint64_t hash = std::strtoull(line.c_str(), &end, 16);
        if (end != line.c_str() + 16) continue;

        Entry entry;
        for (size_t start = 17; start <= line.size();) {
            size_t stop = line.find('\t', start);
            if (stop == std::string::npos) stop = line.size();
            const size_t equals = line.find('=', start);
            if (equals < stop) {
                std::string key = line.substr(start, equals - start);
                std::string value = line.substr(equals + 1, stop - equals - 1);
                if (key == "albedo") entry.albedo = std::move(value);
                else entry.properties.emplace_back(std::move(key), std::move(value));
            }
            start = stop + 1;
        }
        entries_.emplace(hash, std::move(entry));
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
        file << key << "\talbedo=" << entry.albedo;
        for (const auto& [name, value] : entry.properties) file << '\t' << name << '=' << value;
        file << '\n';
    }
    return true;
}

size_t MaterialMap::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}
