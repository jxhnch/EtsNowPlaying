#pragma once
#include "common.h"

#include <memory>
#include <vector>

namespace ep {

enum class CoverState { None, Pending, Found, NotFound };

struct CoverResult {
    CoverState state = CoverState::None;
    std::shared_ptr<const std::vector<uint8_t>> image;  // encoded JPEG/PNG when Found
};

// Looks up the album cover for a radio stream title such as "Artist - Song" on the iTunes Search API.
// Runs on its own thread; asking again for the same title is cheap.
void CoverLookupStart();
void CoverLookupStop();
void CoverLookupRequest(const std::string& streamTitle);
CoverResult CoverLookupGet(const std::string& streamTitle);

}  // namespace ep
