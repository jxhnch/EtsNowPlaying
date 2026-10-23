#include "common.h"

#include "cover_lookup.h"
#include "d3d11_hooks.h"
#include "engine_hooks.h"
#include "log.h"
#include "media.h"
#include "net_hooks.h"
#include "radio_tracker.h"
#include "renderer.h"
#include "safe_mode.h"
#include "screen_assets.h"
#include "settings.h"
#include "version.h"

// The SDK header declares the entry points without extern "C"; the game looks them up by plain name.
extern "C" {
#include <scssdk_telemetry.h>
}

#include <MinHook.h>
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
    ep::SetGameDirs(docs, dir);
    ep::LogInit(dir);
    ep::Logf("EtsNowPlaying v" ETSNP_VERSION ", game_id='%s' game_name='%s' telemetry=%u.%u", gameId.c_str(),
             p->common.game_name ? p->common.game_name : "", SCS_GET_MAJOR_VERSION(version),
             SCS_GET_MINOR_VERSION(version));
    ep::Logf("Texture finder: Ctrl+Shift+F6 start, F7 = GPS painted magenta, F8 = not painted, F9 stop");

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
        bool engine = features.screen && ep::EngineHooksInstall("/home/EtsNowPlaying/gps.tobj", features.disableGameGps);
        bool gfx = ep::D3D11HooksInstall(features.screen, features.upload);
        ep::MediaStart();
        ep::CoverLookupStart();
        ep::RendererStart();
        ep::Logf("init done: net=%d engine=%d d3d11=%d", net, engine, gfx);
        GameLog(SCS_LOG_TYPE_message, "[EtsNowPlaying] started, see Documents\\...\\EtsNowPlaying\\diag.log");
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
    ep::CoverLookupStop();
    ep::MediaStop();
    ep::RadioTrackerShutdown();
    ep::SafeModeEndClean();
    // Hooks stay installed (they pass through) because the module is pinned and the game is exiting.
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
