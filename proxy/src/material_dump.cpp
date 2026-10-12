#include "material_dump.h"

#include "compiler_hook.h"
#include "config.h"
#include "dds.h"
#include "log.h"
#include "texture_capture.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>

namespace {

std::string Quote(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[8];
            snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            out += escaped;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string Hex(uint64_t value) {
    char text[17];
    snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

// Unused constant components can hold anything, NaN included; JSON has no NaN or infinity
std::string Number(float value) {
    if (!std::isfinite(value)) return "null";
    char text[32];
    snprintf(text, sizeof(text), "%.9g", value);
    return text;
}

const char* AddressName(DWORD mode) {
    switch (mode) {
        case D3DTADDRESS_WRAP: return "wrap";
        case D3DTADDRESS_MIRROR: return "mirror";
        case D3DTADDRESS_CLAMP: return "clamp";
        case D3DTADDRESS_BORDER: return "border";
        case D3DTADDRESS_MIRRORONCE: return "mirroronce";
        default: return "unknown";
    }
}

uint64_t HashTexture(const CapturedTexture& texture) {
    uint64_t hash = 14695981039346656037ull;
    auto add = [&hash](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    };
    add(&texture.format, sizeof(texture.format));
    add(&texture.width, sizeof(texture.width));
    add(&texture.height, sizeof(texture.height));
    add(texture.level0.data(), texture.level0.size());
    return hash;
}

bool SameDraw(const DumpedDraw& a, const DumpedDraw& b) {
    if (a.shaderHash != b.shaderHash || a.stage0Texture != b.stage0Texture || a.stage0 != b.stage0 ||
        a.samplers.size() != b.samplers.size()) {
        return false;
    }
    for (size_t i = 0; i < a.samplers.size(); ++i) {
        if (a.samplers[i].texture != b.samplers[i].texture) return false;
    }
    return true;
}

// Writes the dumped textures and describes them as JSON objects
class TextureWriter {
public:
    explicit TextureWriter(std::wstring directory) : directory_(std::move(directory)) {}

    std::string Describe(IDirect3DBaseTexture9* texture) {
        if (!texture) return "null";
        const auto known = described_.find(texture);
        if (known != described_.end()) return known->second;

        std::string json;
        if (const auto captured = FindCapturedTexture(texture)) {
            const std::string name = Hex(HashTexture(*captured)) + ".dds";
            const std::wstring path = directory_ + std::wstring(name.begin(), name.end());
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                const std::vector<uint8_t> dds = MakeDds(captured->format, captured->width, captured->height,
                                                         captured->level0.data(), captured->level0.size());
                std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(dds.data()), dds.size());
                ++written_;
            }
            json = "{\"file\": " + Quote("textures/" + name) + ", \"type\": \"2d\", \"format\": " +
                   Quote(FormatName(captured->format)) + ", \"width\": " + std::to_string(captured->width) +
                   ", \"height\": " + std::to_string(captured->height) + "}";
        } else {
            ++missing_;
            const D3DRESOURCETYPE type = texture->GetType();
            D3DSURFACE_DESC desc = {};
            if (type == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc);
            json = std::string("{\"file\": null, \"type\": ") +
                   (type == D3DRTYPE_TEXTURE ? "\"2d\"" : type == D3DRTYPE_CUBETEXTURE ? "\"cube\"" : "\"volume\"") +
                   ", \"format\": " + Quote(FormatName(desc.Format)) + ", \"width\": " + std::to_string(desc.Width) +
                   ", \"height\": " + std::to_string(desc.Height) +
                   ", \"pool\": " + std::to_string(static_cast<int>(desc.Pool)) +
                   ", \"usage\": " + std::to_string(desc.Usage) +
                   ", \"locks\": " + Quote(DescribeTextureLocks(texture)) + "}";
        }
        described_[texture] = json;
        return json;
    }

    unsigned Written() const { return written_; }
    unsigned Missing() const { return missing_; }

private:
    std::wstring directory_;
    std::map<IDirect3DBaseTexture9*, std::string> described_;
    unsigned written_ = 0;
    unsigned missing_ = 0;
};

} // namespace

void MaterialDump::EndFrame(unsigned frame) {
    if (active_) {
        Write(frame);
        materials_.clear();
        active_ = false;
    }
    if (requested_) {
        requested_ = false;
        active_ = true;
        Log("Material dump of frame %u requested", frame + 1);
    }
}

void MaterialDump::Add(const DumpedDraw& draw) {
    for (Material& material : materials_) {
        if (!SameDraw(material.first, draw)) continue;
        ++material.draws;
        if (material.first.constants != draw.constants) material.constantsVary = true;
        return;
    }
    materials_.push_back({draw, 1, false});
}

void MaterialDump::Write(unsigned frame) {
    const std::wstring directory = ModuleDirectory() + L"roboblitz_remix_material_dumps\\";
    CreateDirectoryW(directory.c_str(), nullptr);
    CreateDirectoryW((directory + L"textures").c_str(), nullptr);

    SYSTEMTIME time;
    GetLocalTime(&time);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d_%02d-%02d-%02d", time.wYear, time.wMonth, time.wDay, time.wHour,
             time.wMinute, time.wSecond);
    const std::string fileName = std::string(stamp) + ".json";

    TextureWriter textures(directory + L"textures\\");
    std::ofstream out(directory + std::wstring(fileName.begin(), fileName.end()), std::ios::binary);
    out << "{\n  \"frame\": " << frame << ",\n  \"time\": " << Quote(stamp) << ",\n  \"materials\": [";

    unsigned known = 0;
    for (size_t m = 0; m < materials_.size(); ++m) {
        const Material& material = materials_[m];
        const DumpedDraw& draw = material.first;
        out << (m ? ",\n" : "\n") << "    {\n";
        out << "      \"shader\": " << Quote(Hex(draw.shaderHash)) << ",\n";
        out << "      \"draws\": " << material.draws << ",\n";
        out << "      \"alphaBlend\": " << (draw.alphaBlend ? "true" : "false") << ",\n";
        out << "      \"stage0\": " << (draw.stage0.empty() ? "null" : Quote(draw.stage0)) << ",\n";
        out << "      \"stage0Texture\": " << textures.Describe(draw.stage0Texture) << ",\n";

        MaterialMap::Entry entry;
        if (GetMaterialMap().Find(draw.shaderHash, entry)) {
            ++known;
            out << "      \"material\": {\n        \"albedo\": " << Quote(entry.albedo);
            for (const auto& [name, value] : entry.properties) out << ",\n        " << Quote(name) << ": " << Quote(value);
            out << "\n      },\n";
        } else {
            out << "      \"material\": null,\n";
        }

        out << "      \"samplers\": {";
        for (size_t s = 0; s < draw.samplers.size(); ++s) {
            const DumpedSampler& sampler = draw.samplers[s];
            out << (s ? ",\n" : "\n") << "        " << Quote(sampler.name) << ": {\"texture\": "
                << textures.Describe(sampler.texture) << ", \"addressU\": " << Quote(AddressName(sampler.addressU))
                << ", \"addressV\": " << Quote(AddressName(sampler.addressV))
                << ", \"srgb\": " << (sampler.srgb ? "true" : "false") << "}";
        }
        out << (draw.samplers.empty() ? "},\n" : "\n      },\n");

        out << "      \"constants\": {";
        for (size_t c = 0; c < draw.constants.size(); ++c) {
            const auto& [name, value] = draw.constants[c];
            out << (c ? ",\n" : "\n") << "        " << Quote(name) << ": [" << Number(value[0]) << ", "
                << Number(value[1]) << ", " << Number(value[2]) << ", " << Number(value[3]) << "]";
        }
        out << (draw.constants.empty() ? "},\n" : "\n      },\n");
        out << "      \"constantsVary\": " << (material.constantsVary ? "true" : "false") << "\n    }";
    }
    out << "\n  ]\n}\n";

    Log("Material dump of frame %u: %zu materials (%u in the material map), %u new textures, %u textures without "
        "data -> roboblitz_remix_material_dumps\\%s",
        frame, materials_.size(), known, textures.Written(), textures.Missing(), fileName.c_str());
}
