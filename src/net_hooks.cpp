#include "net_hooks.h"

#include "log.h"
#include "radio_tracker.h"

#include <MinHook.h>

namespace ep {
namespace {

using SendFn = int(WSAAPI*)(SOCKET, const char*, int, int);
using RecvFn = int(WSAAPI*)(SOCKET, char*, int, int);
using WSASendFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED,
                               LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSARecvFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED,
                               LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using CloseFn = int(WSAAPI*)(SOCKET);

SendFn g_send;
RecvFn g_recv;
WSASendFn g_wsaSend;
WSARecvFn g_wsaRecv;
CloseFn g_close;
std::atomic<bool> g_off{false};

int WSAAPI HookSend(SOCKET s, const char* buf, int len, int flags) {
    if (!g_off && buf && len > 0) RadioOnSend(s, buf, len);
    return g_send(s, buf, len, flags);
}

int WSAAPI HookRecv(SOCKET s, char* buf, int len, int flags) {
    int r = g_recv(s, buf, len, flags);
    if (!g_off && r > 0 && !(flags & MSG_PEEK)) RadioOnRecv(s, buf, r);
    return r;
}

int WSAAPI HookWSASend(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, LPWSAOVERLAPPED ov,
                       LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    if (!g_off && bufs) {
        for (DWORD i = 0; i < count; ++i) {
            if (bufs[i].buf && bufs[i].len > 0) RadioOnSend(s, bufs[i].buf, static_cast<int>(bufs[i].len));
        }
    }
    return g_wsaSend(s, bufs, count, sent, flags, ov, cr);
}

int WSAAPI HookWSARecv(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD received, LPDWORD flags, LPWSAOVERLAPPED ov,
                       LPWSAOVERLAPPED_COMPLETION_ROUTINE cr) {
    int r = g_wsaRecv(s, bufs, count, received, flags, ov, cr);
    // Only synchronous completions: with overlapped I/O the buffers are not filled yet.
    if (!g_off && r == 0 && !ov && received && bufs) {
        DWORD left = *received;
        for (DWORD i = 0; i < count && left > 0; ++i) {
            DWORD n = left < bufs[i].len ? left : bufs[i].len;
            RadioOnRecv(s, bufs[i].buf, static_cast<int>(n));
            left -= n;
        }
    }
    return r;
}

int WSAAPI HookClose(SOCKET s) {
    if (!g_off) RadioOnClose(s);
    return g_close(s);
}

template <typename Fn>
bool Hook(const char* name, void* detour, Fn* original) {
    MH_STATUS st = MH_CreateHookApi(L"ws2_32", name, detour, reinterpret_cast<void**>(original));
    if (st != MH_OK) {
        Logf("NET hook %s failed: %s", name, MH_StatusToString(st));
        return false;
    }
    return true;
}

}  // namespace

bool NetHooksInstall() {
    LoadLibraryW(L"ws2_32.dll");
    bool ok = true;
    ok &= Hook("send", reinterpret_cast<void*>(&HookSend), &g_send);
    ok &= Hook("recv", reinterpret_cast<void*>(&HookRecv), &g_recv);
    ok &= Hook("WSASend", reinterpret_cast<void*>(&HookWSASend), &g_wsaSend);
    ok &= Hook("WSARecv", reinterpret_cast<void*>(&HookWSARecv), &g_wsaRecv);
    ok &= Hook("closesocket", reinterpret_cast<void*>(&HookClose), &g_close);
    if (!ok) return false;
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        Logf("NET MH_EnableHook failed");
        return false;
    }
    Logf("NET hooks installed (send, recv, WSASend, WSARecv, closesocket)");
    return true;
}

void NetHooksDisable() { g_off = true; }

}  // namespace ep
