#pragma once
#include "common.h"

#include <memory>
#include <vector>

namespace ep {

enum class LogoState { None, Pending, Found, NotFound };

struct LogoResult {
    LogoState state = LogoState::None;
    std::shared_ptr<const std::vector<uint8_t>> image;  // encoded PNG/JPEG/ICO when Found
};

// Finds a logo for a radio station (radio-browser.info, first by stream URL, then by name and country) and keeps
// it in <plugin folder>\logos, so every station is looked up only once. Runs on its own thread.
void StationLogoStart();
void StationLogoStop();
void StationLogoRequest(const std::string& stationName, const std::string& country, const std::string& listUrl);
LogoResult StationLogoGet(const std::string& stationName, const std::string& listUrl);

}  // namespace ep
