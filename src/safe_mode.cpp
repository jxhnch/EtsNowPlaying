#include "safe_mode.h"

#include "log.h"

#include <cstdio>

namespace ep {
namespace {

std::wstring g_statePath;

void WriteState(int stage, int running) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", stage);
    WritePrivateProfileStringW(L"state", L"stage", buf, g_statePath.c_str());
    swprintf(buf, 16, L"%d", running);
    WritePrivateProfileStringW(L"state", L"running", buf, g_statePath.c_str());
}

}  // namespace

Features SafeModeBegin(const std::wstring& dir) {
    g_statePath = dir + L"\\state.ini";
    std::wstring ini = dir + L"\\EtsNowPlaying.ini";  // created by LoadSettings

    int stage = static_cast<int>(GetPrivateProfileIntW(L"state", L"stage", 0, g_statePath.c_str()));
    int running = static_cast<int>(GetPrivateProfileIntW(L"state", L"running", 0, g_statePath.c_str()));
    int forced = static_cast<int>(GetPrivateProfileIntW(L"features", L"force_stage", -1, ini.c_str()));

    constexpr int kLastStage = 4;
    if (forced >= 0) {
        stage = forced > kLastStage ? kLastStage : forced;
        Logf("SAFE stage forced to %d by EtsNowPlaying.ini", stage);
    } else if (running) {
        stage = stage < kLastStage ? stage + 1 : kLastStage;
        Logf("SAFE the previous run did not end cleanly (game crash?) -> stage %d", stage);
    } else {
        Logf("SAFE previous run ended cleanly, stage %d", stage);
    }
    WriteState(stage, 1);

    // After a crash the newest and most invasive part goes first: touch input, then frame upload, then the
    // game's own GPS UI patch, finally the whole screen.
    Features f;
    f.stage = stage;
    f.touch = stage < 1;
    f.upload = stage < 2;
    f.disableGameGps = stage < 3;
    f.screen = stage < 4;
    Logf("SAFE features: screen=%d disableGameGps=%d upload=%d touch=%d", f.screen, f.disableGameGps, f.upload, f.touch);
    return f;
}

void SafeModeEndClean() {
    if (g_statePath.empty()) return;
    int stage = static_cast<int>(GetPrivateProfileIntW(L"state", L"stage", 0, g_statePath.c_str()));
    WriteState(stage, 0);
}

}  // namespace ep
