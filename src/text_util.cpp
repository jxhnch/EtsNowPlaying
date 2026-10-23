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

std::wstring NormalizeKey(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (iswalnum(c)) out.push_back(static_cast<wchar_t>(towlower(c)));
    }
    return out;
}

std::wstring StripBrackets(const std::wstring& s) {
    std::wstring out;
    int depth = 0;
    for (wchar_t c : s) {
        if (c == L'(' || c == L'[') ++depth;
        else if ((c == L')' || c == L']') && depth > 0) --depth;
        else if (depth == 0) out.push_back(c);
    }
    return out;
}

std::wstring FirstArtist(const std::wstring& s) {
    std::wstring lower;
    for (wchar_t c : s) lower.push_back(static_cast<wchar_t>(towlower(c)));
    size_t cut = std::wstring::npos;
    for (const wchar_t* sep : {L",", L" & ", L" and ", L" feat", L" ft.", L" ft ", L" featuring ", L" x ", L"/", L" + ", L" with ", L" vs"}) {
        size_t p = lower.find(sep);
        if (p != std::wstring::npos && p > 0 && p < cut) cut = p;
    }
    std::wstring first = cut == std::wstring::npos ? s : s.substr(0, cut);
    while (!first.empty() && iswspace(first.back())) first.pop_back();
    return first;
}

std::wstring BaseTitle(const std::wstring& s) {
    std::wstring t = StripBrackets(s);
    size_t dash = t.find(L" - ");
    if (dash != std::wstring::npos && dash > 0) t = t.substr(0, dash);  // "Wonderwall - Remastered"
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    return t;
}

bool SimilarKeys(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos;
}

std::string UrlEncode(const std::string& utf8) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

bool JsonString(const std::string& obj, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\":\"";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    out.clear();
    while (p < obj.size() && obj[p] != '"') {
        if (obj[p] == '\\' && p + 1 < obj.size()) {
            char e = obj[p + 1];
            if (e == 'u' && p + 5 < obj.size()) {
                unsigned code = static_cast<unsigned>(strtoul(obj.substr(p + 2, 4).c_str(), nullptr, 16));
                out += ToUtf8(std::wstring(1, static_cast<wchar_t>(code)));
                p += 6;
                continue;
            }
            out.push_back(e == 'n' ? '\n' : e);
            p += 2;
            continue;
        }
        out.push_back(obj[p++]);
    }
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
