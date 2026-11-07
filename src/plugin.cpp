#include "common.h"

#include "cover_lookup.h"
#include "d3d11_hooks.h"
#include "engine_hooks.h"
#include "log.h"
#include "media.h"
#include "net_hooks.h"
#include "radio_tracker.h"
#include "renderer.h"
#include "station_logo.h"
#include "safe_mode.h"
#include "screen_assets.h"
#include "screen_fit.h"
#include "settings.h"
#include "text_util.h"
#include "version.h"

// The SDK header declares the entry points without extern "C"; the game looks them up by plain name.
extern "C" {
#include <scssdk_telemetry.h>
}

#include <MinHook.h>
#include <thread>
#include <shlobj.h>

namespace {

scs_log_t g_scsLog = nullptr;

void GameLog(scs_log_type_t type, const char* msg) {
    if (g_scsLog) g_scsLog(type, msg);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out;
}

// ---- game updates ----------------------------------------------------------------------------------------------------
// A game update can move the places the plugin hooks into. Two safety nets keep the player from being left with a dead
// GPS screen:
//  * if a hook cannot be found at all, the game's own GPS stays untouched and the game console says so;
//  * if the hooks exist but the GPS texture is never redirected within a minute after the truck was loaded, the game's GPS
//    is switched back on, the game console says so, and the game version is remembered (together with the plugin version).
//    A remembered version starts in "observe only" mode: nothing is changed in the game; as soon as a lookup of the GPS
//    texture shows that redirecting would work, the mark is removed and the player is asked to restart the game.
const char kGithub[] = "https://github.com/jxhnch/EtsNowPlaying";

std::wstring g_dir;
std::string g_gameName;
std::atomic<ULONGLONG> g_truckTick{0};  // when the player's truck was configured for the first time
bool g_decided = false;
bool g_observe = false;
ULONGLONG g_lastCheck = 0;

// What the player is told. Nobody reads the game console, so it is a small pop-up window (shown from its own thread, the
// game keeps running). The "not supported" one comes once per game + plugin version, not at every start.
enum class Notice { None, Unsupported, WorksAgain };
Notice g_notice = Notice::None;
bool g_noticeShown = false;

std::wstring StatePath() { return g_dir + L"\\state.ini"; }

bool CompatFlagged() {
    wchar_t game[256] = {}, plugin[64] = {};
    GetPrivateProfileStringW(L"compat", L"game", L"", game, 256, StatePath().c_str());
    GetPrivateProfileStringW(L"compat", L"plugin", L"", plugin, 64, StatePath().c_str());
    const bool failed = GetPrivateProfileIntW(L"compat", L"failed", 0, StatePath().c_str()) != 0;
    return failed && ep::ToWide(g_gameName) == game && ep::ToWide(ETSNP_VERSION) == plugin;
}

void CompatSet(bool failed) {
    if (!failed) {
        WritePrivateProfileStringW(L"compat", nullptr, nullptr, StatePath().c_str());
        return;
    }
    WritePrivateProfileStringW(L"compat", L"game", ep::ToWide(g_gameName).c_str(), StatePath().c_str());
    WritePrivateProfileStringW(L"compat", L"plugin", ep::ToWide(ETSNP_VERSION).c_str(), StatePath().c_str());
    WritePrivateProfileStringW(L"compat", L"failed", L"1", StatePath().c_str());
}

bool NoticeAlreadyGiven() {
    wchar_t game[256] = {}, plugin[64] = {};
    GetPrivateProfileStringW(L"compat", L"game", L"", game, 256, StatePath().c_str());
    GetPrivateProfileStringW(L"compat", L"plugin", L"", plugin, 64, StatePath().c_str());
    return GetPrivateProfileIntW(L"compat", L"noticed", 0, StatePath().c_str()) != 0 && ep::ToWide(g_gameName) == game &&
           ep::ToWide(ETSNP_VERSION) == plugin;
}

void MarkNoticed() {
    WritePrivateProfileStringW(L"compat", L"game", ep::ToWide(g_gameName).c_str(), StatePath().c_str());
    WritePrivateProfileStringW(L"compat", L"plugin", ep::ToWide(ETSNP_VERSION).c_str(), StatePath().c_str());
    WritePrivateProfileStringW(L"compat", L"noticed", L"1", StatePath().c_str());
}

void WarnUnsupported() {
    const std::string msg = std::string("[EtsNowPlaying] Plugin ") + ETSNP_VERSION + " does not support this game version (" + g_gameName +
                            ") yet, the normal GPS is used. Please look for an update: " + kGithub;
    GameLog(SCS_LOG_TYPE_warning, msg.c_str());
    if (g_notice == Notice::None) g_notice = Notice::Unsupported;
}

void ShowNotice(Notice kind) {
    const bool german = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
    std::wstring text;
    if (kind == Notice::Unsupported) {
        const std::wstring game = ep::ToWide(g_gameName), url = ep::ToWide(std::string(kGithub) + "/releases");
        text = german ? L"EtsNowPlaying unterstützt diese Spielversion noch nicht (" + game +
                            L").\n\nDein normales GPS bleibt in Benutzung, sonst ist nichts beeinträchtigt.\n"
                            L"Ein Plugin-Update gibt es hier:\n" + url
                      : L"EtsNowPlaying does not support this version of the game yet (" + game +
                            L").\n\nYour normal GPS stays in use, nothing else is affected.\n"
                            L"A plugin update will appear here:\n" + url;
    } else {
        text = german ? L"EtsNowPlaying funktioniert mit dieser Spielversion wieder.\n\nBitte starte das Spiel einmal neu."
                      : L"EtsNowPlaying works with this game version again.\n\nPlease restart the game once.";
    }
    // Own thread: MessageBox only blocks that thread, the game keeps running.
    std::thread([text] {
        MessageBoxW(nullptr, text.c_str(), L"EtsNowPlaying", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    }).detach();
}

// Shows a pending notice once the player is really in the game (the truck was loaded a few seconds ago).
void ProcessNotice(ULONGLONG now) {
    if (g_notice == Notice::None || g_noticeShown) return;
    if (g_notice == Notice::Unsupported) {
        const ULONGLONG truck = g_truckTick.load();
        if (!truck || now - truck < 8000) return;
        g_noticeShown = true;
        if (NoticeAlreadyGiven()) return;
        MarkNoticed();
    }
    g_noticeShown = true;
    ShowNotice(g_notice);
}

// Called once per frame (game thread), cheap once the decision is made.
void WatchTick() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastCheck < 1000) return;
    g_lastCheck = now;
    ProcessNotice(now);
    if (g_decided) return;
    if (g_observe) {
        if (ep::EngineGpsSightings() > 0) {
            g_decided = true;
            CompatSet(false);
            ep::Logf("COMPAT the GPS texture lookup was seen again: this game version works with this plugin, mark removed");
            GameLog(SCS_LOG_TYPE_message, "[EtsNowPlaying] This game version works with the plugin again. Please restart the game once.");
            g_notice = Notice::WorksAgain;
            g_noticeShown = false;
        }
        return;
    }
    const ULONGLONG truck = g_truckTick.load();
    if (!truck || now - truck < 60000) return;
    g_decided = true;
    if (ep::EngineRedirects() > 0) {
        ep::Logf("COMPAT the GPS texture was redirected %d time(s): fine", ep::EngineRedirects());
        return;
    }
    ep::Logf("COMPAT no GPS texture redirect within a minute of the truck being loaded: game version '%s' is probably not supported yet",
             g_gameName.c_str());
    ep::EngineRestoreGameGps();
    CompatSet(true);
    WarnUnsupported();
}

SCSAPI_VOID OnFrameEnd(const scs_event_t, const void* const, const scs_context_t) { WatchTick(); }

SCSAPI_VOID OnConfiguration(const scs_event_t, const void* const info, const scs_context_t) {
    const auto* c = static_cast<const scs_telemetry_configuration_t*>(info);
    if (c && c->id && strcmp(c->id, "truck") == 0 && g_truckTick.load() == 0) g_truckTick = GetTickCount64();
}

}  // namespace

extern "C" SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version,
                                            const scs_telemetry_init_params_t* const params) {
    if (version != SCS_TELEMETRY_VERSION_1_00 && version != SCS_TELEMETRY_VERSION_1_01) return SCS_RESULT_unsupported;
    const auto* p = static_cast<const scs_telemetry_init_params_v100_t*>(params);
    g_scsLog = p->common.log;

    // Never let the game unload us while hooks point into this module.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&scs_telemetry_init), &self);

    std::string gameId = p->common.game_id ? p->common.game_id : "";
    const wchar_t* docsName = gameId == "ats" ? L"American Truck Simulator" : L"Euro Truck Simulator 2";
    std::wstring docs = KnownFolder(FOLDERID_Documents) + L"\\" + docsName;
    std::wstring dir = docs + L"\\EtsNowPlaying";
    g_dir = dir;
    g_gameName = p->common.game_name ? p->common.game_name : "";
    ep::SetGameDirs(docs, dir);
    ep::LogInit(dir);
    ep::Logf("EtsNowPlaying v" ETSNP_VERSION ", game_id='%s' game_name='%s' telemetry=%u.%u", gameId.c_str(),
             p->common.game_name ? p->common.game_name : "", SCS_GET_MAJOR_VERSION(version),
             SCS_GET_MINOR_VERSION(version));

    try {
        MH_STATUS st = MH_Initialize();
        if (st != MH_OK) {
            ep::Logf("MH_Initialize failed: %s", MH_StatusToString(st));
            GameLog(SCS_LOG_TYPE_error, "[EtsNowPlaying] MinHook init failed");
            return SCS_RESULT_ok;  // never block the game because of us
        }
        ep::LoadSettings(dir);
        ep::Features features = ep::SafeModeBegin(dir);
        ep::EnsureScreenAssets(dir);
        ep::RadioTrackerInit();
        bool net = ep::NetHooksInstall();
        // A game version that failed before (see WatchTick) is only observed, nothing is changed in the game.
        g_observe = features.screen && CompatFlagged() &&
                    GetPrivateProfileIntW(L"features", L"force_stage", -1, (dir + L"\\EtsNowPlaying.ini").c_str()) < 0;
        if (g_observe) {
            ep::Logf("COMPAT game version '%s' was flagged as unsupported by plugin %s: observing only", g_gameName.c_str(), ETSNP_VERSION);
            ep::EngineSetObserveOnly(true);
            features.disableGameGps = false;
            WarnUnsupported();
        }
        bool engine = features.screen && ep::EngineHooksInstall("/home/EtsNowPlaying/gps.tobj", features.disableGameGps);
        if (features.screen && !engine) {
            ep::Logf("COMPAT the hooks could not be installed: game version '%s' is not supported by plugin %s yet", g_gameName.c_str(),
                     ETSNP_VERSION);
            WarnUnsupported();
            g_decided = true;
        }
        ep::FitInit(features.screen && features.touch ? ep::GetSettings().autoFit : 0);
        bool gfx = ep::D3D11HooksInstall(features.screen, features.upload, features.touch && ep::GetSettings().touch);
        ep::MediaStart();
        ep::CoverLookupStart();
        ep::StationLogoStart();
        ep::RendererStart();
        ep::Logf("init done: net=%d engine=%d d3d11=%d", net, engine, gfx);
        GameLog(SCS_LOG_TYPE_message, "[EtsNowPlaying] started, see Documents\\...\\EtsNowPlaying\\diag.log");
        if (p->register_for_event) {
            p->register_for_event(SCS_TELEMETRY_EVENT_configuration, OnConfiguration, nullptr);
            p->register_for_event(SCS_TELEMETRY_EVENT_frame_end, OnFrameEnd, nullptr);
        }
    } catch (...) {
        ep::Logf("exception during init");
    }
    return SCS_RESULT_ok;
}

extern "C" SCSAPI_VOID scs_telemetry_shutdown(void) {
    ep::Logf("shutdown");
    ep::NetHooksDisable();
    ep::EngineHooksDisable();
    ep::D3D11HooksDisable();
    ep::RendererStop();
    ep::StationLogoStop();
    ep::CoverLookupStop();
    ep::MediaStop();
    ep::RadioTrackerShutdown();
    ep::SafeModeEndClean();
    // Hooks stay installed (they pass through) because the module is pinned and the game is exiting.
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
