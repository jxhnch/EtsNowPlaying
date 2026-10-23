#include "text_util.h"

#include <algorithm>

namespace ep {

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        cp = 1252;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), nullptr, 0);
    }
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(cp, flags, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool SplitArtistTitle(const std::string& raw, std::wstring& artist, std::wstring& title) {
    std::wstring w = ToWide(raw);
    for (wchar_t& c : w) {
        switch (c) {
            case 0x00A0: case 0x2002: case 0x2003: case 0x2007: case 0x2009: case 0x200A: case 0x202F: case 0x3000:
                c = L' ';
                break;
            case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: case 0x02D7:
            case 0x2043: case 0xFE58: case 0xFE63: case 0xFF0D:
                c = L'-';
                break;
            default:
                break;
        }
    }
    auto trim = [](std::wstring s) {
        size_t b = 0, e = s.size();
        while (b < e && iswspace(s[b])) ++b;
        while (e > b && iswspace(s[e - 1])) --e;
        return s.substr(b, e - b);
    };
    size_t p = w.find(L" - ");
    artist.clear();
    title = trim(w);
    if (p == std::wstring::npos) return false;
    std::wstring a = trim(w.substr(0, p)), t = trim(w.substr(p + 3));
    if (a.empty() || t.empty()) return false;
    artist = a;
    title = t;
    return true;
}

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace ep
