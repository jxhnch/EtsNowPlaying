#include "settings.h"

#include "log.h"

#include <cstdio>

namespace ep {
namespace {

Settings g_settings;

void EnsureKey(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[8];
    GetPrivateProfileStringW(section, key, L"\x01", buf, 8, ini.c_str());
    if (buf[0] == L'\x01') WritePrivateProfileStringW(section, key, value, ini.c_str());
}

// Keys of older builds that are not user settings any more: removed unless somebody changed them.
void DropIfDefault(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int defaultValue) {
    const int marker = -987654;
    int v = static_cast<int>(GetPrivateProfileIntW(section, key, marker, ini.c_str()));
    if (v == defaultValue) WritePrivateProfileStringW(section, key, nullptr, ini.c_str());
}

}  // namespace

const Settings& GetSettings() { return g_settings; }

void LoadSettings(const std::wstring& dir) {
    std::wstring ini = dir + L"\\EtsNowPlaying.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = _wfopen(ini.c_str(), L"wb");
        if (f) {
            const char text[] =
                "; EtsNowPlaying settings\r\n"
                "\r\n"
                "[screen]\r\n"
                "; 1 = flip the picture vertically. Set to 0 if the screen is upside down.\r\n"
                "flip_vertical=1\r\n"
                "; Experimental: 1 = fit the picture to the shape of each truck's display. 2 = only measure and log. 0 = off.\r\n"
                "auto_fit=0\r\n"
                "\r\n"
                "[media]\r\n"
                "; Only show media from apps whose name contains this text. Empty = any player.\r\n"
                "app_filter=spotify\r\n"
                "\r\n"
                "[radio]\r\n"
                "; 1 = look up song covers and station logos online (iTunes, Deezer, radio-browser.info). 0 = off.\r\n"
                "cover_lookup=1\r\n"
                "\r\n"
                "[touch]\r\n"
                "; 1 = the buttons on the screen can be clicked with the mouse.\r\n"
                "enabled=1\r\n";
            fwrite(text, 1, sizeof(text) - 1, f);
            fclose(f);
        }
    }
    // Upgrades from older builds.
    EnsureKey(ini, L"screen", L"flip_vertical", L"1");
    EnsureKey(ini, L"screen", L"auto_fit", L"1");
    EnsureKey(ini, L"media", L"app_filter", L"spotify");
    EnsureKey(ini, L"radio", L"cover_lookup", L"1");
    EnsureKey(ini, L"touch", L"enabled", L"1");
    WritePrivateProfileStringW(L"beta", L"touch", nullptr, ini.c_str());
    WritePrivateProfileStringW(L"radio", L"stale_ms", nullptr, ini.c_str());
    WritePrivateProfileStringW(L"radio", L"hold_ms", nullptr, ini.c_str());
    DropIfDefault(ini, L"radio", L"pause_after_ms", 2500);
    DropIfDefault(ini, L"radio", L"switch_hold_ms", 600);
    DropIfDefault(ini, L"features", L"force_stage", -1);

    g_settings.flipVertical = GetPrivateProfileIntW(L"screen", L"flip_vertical", 1, ini.c_str()) != 0;
    g_settings.coverLookup = GetPrivateProfileIntW(L"radio", L"cover_lookup", 1, ini.c_str()) != 0;
    g_settings.radioStaleMs = static_cast<int>(GetPrivateProfileIntW(L"radio", L"pause_after_ms", 2500, ini.c_str()));
    g_settings.radioHoldMs = static_cast<int>(GetPrivateProfileIntW(L"radio", L"switch_hold_ms", 600, ini.c_str()));
    g_settings.touch = GetPrivateProfileIntW(L"touch", L"enabled", 1, ini.c_str()) != 0;
    g_settings.textureFinder = GetPrivateProfileIntW(L"debug", L"texture_finder", 0, ini.c_str()) != 0;
    g_settings.autoFit = static_cast<int>(GetPrivateProfileIntW(L"screen", L"auto_fit", 1, ini.c_str()));

    wchar_t filter[128] = {};
    GetPrivateProfileStringW(L"media", L"app_filter", L"spotify", filter, 128, ini.c_str());
    int n = WideCharToMultiByte(CP_UTF8, 0, filter, -1, nullptr, 0, nullptr, nullptr);
    std::string f(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, filter, -1, f.data(), n, nullptr, nullptr);
    for (auto& c : f) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    g_settings.appFilter = f;

    Logf("SETTINGS flip_vertical=%d auto_fit=%d app_filter='%s' cover_lookup=%d touch=%d", g_settings.flipVertical,
         g_settings.autoFit, g_settings.appFilter.c_str(), g_settings.coverLookup, g_settings.touch);
}

}  // namespace ep
