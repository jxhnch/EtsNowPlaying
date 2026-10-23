#include "sigscan.h"

#include <vector>

namespace ep {
namespace {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;  // 1 = must match
};

bool Parse(const char* text, Pattern& out) {
    const char* p = text;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') {
            out.bytes.push_back(0);
            out.mask.push_back(0);
            p += 2;
            continue;
        }
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = hex(p[0]), lo = hex(p[1]);
        if (hi < 0 || lo < 0) return false;
        out.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
        out.mask.push_back(1);
        p += 2;
    }
    return !out.bytes.empty();
}

}  // namespace

uint8_t* FindUniquePattern(HMODULE module, const char* pattern, int* hits) {
    if (hits) *hits = 0;
    Pattern pat;
    if (!module || !Parse(pattern, pat)) return nullptr;

    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    // The first non-wildcard byte lets us skip most positions quickly.
    size_t anchor = 0;
    while (anchor < pat.mask.size() && !pat.mask[anchor]) ++anchor;
    if (anchor == pat.mask.size()) return nullptr;

    uint8_t* found = nullptr;
    int count = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* start = base + sec[i].VirtualAddress;
        size_t size = sec[i].Misc.VirtualSize;
        if (size < pat.bytes.size()) continue;
        size_t last = size - pat.bytes.size();
        for (size_t off = 0; off <= last; ++off) {
            if (start[off + anchor] != pat.bytes[anchor]) continue;
            bool ok = true;
            for (size_t k = 0; k < pat.bytes.size(); ++k) {
                if (pat.mask[k] && start[off + k] != pat.bytes[k]) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                ++count;
                found = const_cast<uint8_t*>(start + off);
            }
        }
    }
    if (hits) *hits = count;
    return count == 1 ? found : nullptr;
}

}  // namespace ep
