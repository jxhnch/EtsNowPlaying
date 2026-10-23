#include "http.h"

#include <winhttp.h>

namespace ep {
namespace {

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle = nullptr) : h(handle) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

}  // namespace

bool HttpGet(const std::wstring& url, std::vector<uint8_t>& body, int timeoutMs, size_t maxBytes) {
    body.clear();

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) return false;

    Handle session(WinHttpOpen(L"EtsNowPlaying/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) {
        session.h = WinHttpOpen(L"EtsNowPlaying/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session.h) return false;
    }
    WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    Handle connection(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!connection.h) return false;
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle request(WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.h) return false;

    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        return false;
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) return false;
        if (available == 0) break;
        if (body.size() + available > maxBytes) return false;
        size_t old = body.size();
        body.resize(old + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, body.data() + old, available, &read)) return false;
        body.resize(old + read);
    }
    return true;
}

}  // namespace ep
