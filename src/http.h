#pragma once
#include "common.h"

#include <vector>

namespace ep {

// Minimal blocking HTTP(S) GET via WinHTTP (follows redirects, honours the system proxy).
// Returns true on a 200 response; the body is limited to `maxBytes`.
bool HttpGet(const std::wstring& url, std::vector<uint8_t>& body, int timeoutMs = 6000, size_t maxBytes = 8u * 1024 * 1024);

}  // namespace ep
