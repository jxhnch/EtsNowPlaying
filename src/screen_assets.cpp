#include "screen_assets.h"

#include "log.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace ep {
namespace {

bool WriteIfDifferent(const std::wstring& path, const std::vector<uint8_t>& data) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) && attr.nFileSizeLow == data.size() &&
        attr.nFileSizeHigh == 0) {
        return true;
    }
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

void Put32(std::vector<uint8_t>& v, size_t at, uint32_t value) { memcpy(&v[at], &value, 4); }

}  // namespace

bool EnsureScreenAssets(const std::wstring& dir) {
    // DDS: 64x2048, BC3 (DXT5), one mip. The content is irrelevant, the hook swaps the texture.
    const uint32_t dataSize = kPlaceholderWidth / 4 * (kPlaceholderHeight / 4) * 16;
    std::vector<uint8_t> dds(128 + dataSize, 0);
    memcpy(&dds[0], "DDS ", 4);
    Put32(dds, 4, 124);                 // header size
    Put32(dds, 8, 0x000A1007);          // CAPS | HEIGHT | WIDTH | PIXELFORMAT | MIPMAPCOUNT | LINEARSIZE
    Put32(dds, 12, kPlaceholderHeight);
    Put32(dds, 16, kPlaceholderWidth);
    Put32(dds, 20, dataSize);
    Put32(dds, 28, 1);                  // mip levels
    Put32(dds, 76, 32);                 // pixel format size
    Put32(dds, 80, 4);                  // DDPF_FOURCC
    memcpy(&dds[84], "DXT5", 4);
    Put32(dds, 108, 0x1000);            // DDSCAPS_TEXTURE
    for (size_t o = 128; o + 16 <= dds.size(); o += 16) {
        dds[o] = 0xFF;                  // opaque alpha block, black colour block
        dds[o + 1] = 0xFF;
    }

    // TOBJ: fixed 0x28 byte header (2D texture, linear filtering, clamped), then a u64 length and the DDS path.
    static const uint8_t header[0x28] = {0x01, 0x0A, 0xB1, 0x70, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                         0,    0,    0,    0,    1, 0, 0, 0, 2, 0, 3, 3, 2, 0, 2, 2,
                                         0,    1,    0,    0,    0, 1, 1, 0};
    // Must be exactly as long as the game's own path ("/vehicle/truck/share/gps.tobj") or shorter.
    const char ddsPath[] = "/home/EtsNowPlaying/gps.dds";
    std::vector<uint8_t> tobj(sizeof(header) + 8 + sizeof(ddsPath) - 1);
    memcpy(&tobj[0], header, sizeof(header));
    uint64_t len = sizeof(ddsPath) - 1;
    memcpy(&tobj[sizeof(header)], &len, 8);
    memcpy(&tobj[sizeof(header) + 8], ddsPath, len);

    bool ok = WriteIfDifferent(dir + L"\\gps.dds", dds) && WriteIfDifferent(dir + L"\\gps.tobj", tobj);
    if (!ok) Logf("ASSETS could not write the placeholder files into the plugin folder");
    return ok;
}

}  // namespace ep
