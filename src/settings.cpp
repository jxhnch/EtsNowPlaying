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

}  // namespace

const Settings& GetSettings() { return g_settings; }

void LoadSettings(const std::wstring& dir) {
    std::wstring ini = dir + L"\\EtsNowPlaying.ini";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = _wfopen(ini.c_str(), L"wb");
        if (f) {
            const char text[] =
                "; EtsNowPlaying settings. Delete a line to get its default back.\r\n"
                "[features]\r\n"
                "; -1 = automatic: after a game crash the plugin switches parts off step by step.\r\n"
                ";  0 = everything on, 1 = no touch input, 2 = also no frame upload, 3 = also keep the game's own\r\n"
                ";  GPS UI, 4 = display off (log only). Set 0..4 to force a stage and disable the automatic fallback.\r\n";
            fwrite(text, 1, sizeof(text) - 1, f);
            fclose(f);
        }
    }
    EnsureKey(ini, L"features", L"force_stage", L"-1");
    EnsureKey(ini, L"screen", L"flip_vertical", L"1");
    EnsureKey(ini, L"media", L"app_filter", L"spotify");
    EnsureKey(ini, L"radio", L"cover_lookup", L"1");
    // Older builds wrote stale_ms / hold_ms with slower values; the new keys replace them.
    WritePrivateProfileStringW(L"radio", L"stale_ms", nullptr, ini.c_str());
    WritePrivateProfileStringW(L"radio", L"hold_ms", nullptr, ini.c_str());
    EnsureKey(ini, L"radio", L"pause_after_ms", L"2500");
    EnsureKey(ini, L"radio", L"switch_hold_ms", L"600");
    EnsureKey(ini, L"beta", L"touch", L"1");

    g_settings.flipVertical = GetPrivateProfileIntW(L"screen", L"flip_vertical", 1, ini.c_str()) != 0;
    g_settings.coverLookup = GetPrivateProfileIntW(L"radio", L"cover_lookup", 1, ini.c_str()) != 0;
    g_settings.radioStaleMs = static_cast<int>(GetPrivateProfileIntW(L"radio", L"pause_after_ms", 2500, ini.c_str()));
    g_settings.radioHoldMs = static_cast<int>(GetPrivateProfileIntW(L"radio", L"switch_hold_ms", 600, ini.c_str()));
    g_settings.touch = GetPrivateProfileIntW(L"beta", L"touch", 1, ini.c_str()) != 0;

    wchar_t filter[128] = {};
    GetPrivateProfileStringW(L"media", L"app_filter", L"spotify", filter, 128, ini.c_str());
    int n = WideCharToMultiByte(CP_UTF8, 0, filter, -1, nullptr, 0, nullptr, nullptr);
    std::string f(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, filter, -1, f.data(), n, nullptr, nullptr);
    for (auto& c : f) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    g_settings.appFilter = f;

    Logf("SETTINGS flip_vertical=%d app_filter='%s' cover_lookup=%d pause_after_ms=%d switch_hold_ms=%d touch=%d",
         g_settings.flipVertical, g_settings.appFilter.c_str(), g_settings.coverLookup, g_settings.radioStaleMs,
         g_settings.radioHoldMs, g_settings.touch);
}

}  // namespace ep
