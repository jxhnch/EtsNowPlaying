#include "engine_hooks.h"

#include "log.h"
#include "sigscan.h"

#include <MinHook.h>
#include <intrin.h>

#include <cstring>
#include <string>

namespace ep {
namespace {

// Start of the function that processes the queue of pending texture loads.
// Layout of the queue object (`this`): +0x170 first list node, +0x180 list end marker, +0x190 spin lock.
// Layout of a list node: +0x00 next, +0x10 request. Request: +0x40 char* path, +0x48 int length.
const char* kQueueSignature =
    "48 8B C4 48 89 58 18 48 89 70 20 55 57 41 54 41 56 41 57 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 48 8B F9";

// `mov rdx,[r14+x]; test rdx,rdx; je <skip GPSWindow>; mov r8,[r14+y]; lea rax,[rip+"GPSWindow"]`
// The je at +10 decides whether the cabin GPS UI is rendered.
const char* kGpsWindowSignature =
    "49 8B 96 ?? ?? ?? ?? 48 85 D2 0F 84 ?? ?? ?? ?? 4D 8B 86 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ??";

const char kGameGpsPath[] = "/vehicle/truck/share/gps.tobj";

using QueueFn = uint64_t(__fastcall*)(void*);
QueueFn g_origQueue = nullptr;

std::atomic<bool> g_off{false};
char g_override[128] = {};
size_t g_overrideLen = 0;
uint8_t* g_gpsJump = nullptr;
std::atomic<bool> g_gpsPatched{false};
std::atomic<int> g_rewrites{0};

bool SafeStringEquals(const char* p, const char* expected) {
    __try {
        return strcmp(p, expected) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Rewrites queued requests for the game's GPS texture. Runs with the queue's spin lock held.
int RewriteQueued(uint8_t* self) {
    int n = 0;
    __try {
        uint8_t* end = self + 0x180;
        uint8_t* node = *reinterpret_cast<uint8_t**>(self + 0x170);
        for (int guard = 0; node && node != end && guard < 4096; ++guard, node = *reinterpret_cast<uint8_t**>(node)) {
            uint8_t* request = *reinterpret_cast<uint8_t**>(node + 0x10);
            if (!request) continue;
            char* path = *reinterpret_cast<char**>(request + 0x40);
            if (!path || strcmp(path, kGameGpsPath) != 0) continue;
            // Our path is not longer than the original, so it fits into the existing buffer.
            memcpy(path, g_override, g_overrideLen + 1);
            *reinterpret_cast<int*>(request + 0x48) = static_cast<int>(g_overrideLen);
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = -1;
    }
    return n;
}

uint64_t __fastcall HookQueue(void* self) {
    if (!g_off && self) {
        volatile char* lock = reinterpret_cast<volatile char*>(static_cast<uint8_t*>(self) + 0x190);
        bool locked = false;
        __try {
            // Same test-and-set spin lock the game uses around its queue.
            while (_InterlockedExchange8(const_cast<char*>(lock), 1) != 0) {
                while (*lock) _mm_pause();
            }
            locked = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (locked) {
            int n = RewriteQueued(static_cast<uint8_t*>(self));
            _InterlockedExchange8(const_cast<char*>(lock), 0);
            if (n > 0) {
                int total = g_rewrites.fetch_add(n) + n;
                Logf("ENGINE redirected %d texture request(s) for the cabin GPS to '%s' (total %d)", n, g_override, total);
            } else if (n < 0) {
                Logf("ENGINE queue walk raised an exception, layout changed?");
            }
        }
    }
    return g_origQueue(self);
}

}  // namespace

bool EngineHooksInstall(const char* overridePath, bool disableGameGps) {
    g_overrideLen = strlen(overridePath);
    if (g_overrideLen >= sizeof(g_override) || g_overrideLen > strlen(kGameGpsPath)) {
        Logf("ENGINE override path '%s' must not be longer than '%s'", overridePath, kGameGpsPath);
        return false;
    }
    memcpy(g_override, overridePath, g_overrideLen + 1);

    HMODULE exe = GetModuleHandleW(nullptr);

    int hits = 0;
    uint8_t* queue = FindUniquePattern(exe, kQueueSignature, &hits);
    if (!queue) {
        Logf("ENGINE texture queue signature: %d matches (need exactly 1) - game updated? GPS display disabled", hits);
        return false;
    }
    uint8_t* gps = FindUniquePattern(exe, kGpsWindowSignature, &hits);
    if (gps) {
        // Sanity check: the lea after the jump must point at the string "GPSWindow".
        int32_t rel = *reinterpret_cast<int32_t*>(gps + 26);
        const char* name = reinterpret_cast<const char*>(gps + 30 + rel);
        if (gps[10] == 0x0F && gps[11] == 0x84 && SafeStringEquals(name, "GPSWindow")) {
            g_gpsJump = gps + 10;
        } else {
            Logf("ENGINE GPS window signature matched but the string check failed - not patching");
        }
    } else {
        Logf("ENGINE GPS window signature: %d matches (need exactly 1)", hits);
    }

    MH_STATUS st = MH_CreateHook(queue, reinterpret_cast<void*>(&HookQueue), reinterpret_cast<void**>(&g_origQueue));
    if (st != MH_OK) {
        Logf("ENGINE hook failed: %s", MH_StatusToString(st));
        return false;
    }
    st = MH_EnableHook(queue);
    if (st != MH_OK) {
        Logf("ENGINE enable failed: %s", MH_StatusToString(st));
        return false;
    }
    Logf("ENGINE hooks ready: queue fn at exe+0x%llx, GPS window jump %s", static_cast<unsigned long long>(queue - reinterpret_cast<uint8_t*>(exe)),
         g_gpsJump ? "found" : "NOT found");
    // Same order as the original approach: the game's GPS UI is off before any truck is loaded.
    if (disableGameGps) EngineDisableGameGps();
    return true;
}

void EngineDisableGameGps() {
    if (!g_gpsJump || g_gpsPatched.exchange(true)) return;
    DWORD old = 0;
    if (!VirtualProtect(g_gpsJump, 2, PAGE_EXECUTE_READWRITE, &old)) {
        Logf("ENGINE VirtualProtect failed (%lu)", GetLastError());
        g_gpsPatched = false;
        return;
    }
    // 0F 84 rel32 (je) -> 90 E9 rel32 (nop; jmp): same target, always taken.
    _InterlockedExchange16(reinterpret_cast<short*>(g_gpsJump), static_cast<short>(0xE990));
    VirtualProtect(g_gpsJump, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_gpsJump, 2);
    Logf("ENGINE game GPS window switched off");
}

void EngineHooksDisable() {
    g_off = true;
    if (g_gpsJump && g_gpsPatched.exchange(false)) {
        DWORD old = 0;
        if (VirtualProtect(g_gpsJump, 2, PAGE_EXECUTE_READWRITE, &old)) {
            _InterlockedExchange16(reinterpret_cast<short*>(g_gpsJump), static_cast<short>(0x840F));
            VirtualProtect(g_gpsJump, 2, old, &old);
            FlushInstructionCache(GetCurrentProcess(), g_gpsJump, 2);
        }
    }
}

}  // namespace ep
