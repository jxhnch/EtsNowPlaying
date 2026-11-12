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
// The same code with a short jump (the compiler picked that form for a neighbouring spot in game 1.62).
const char* kGpsWindowSignatureShort =
    "49 8B 96 ?? ?? ?? ?? 48 85 D2 74 ?? 4D 8B 86 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ??";

const char kGameGpsPath[] = "/vehicle/truck/share/gps.tobj";

using QueueFn = uint64_t(__fastcall*)(void*);
QueueFn g_origQueue = nullptr;

std::atomic<bool> g_off{false};
char g_override[128] = {};
size_t g_overrideLen = 0;
uint8_t* g_gpsJump = nullptr;
int g_gpsLen = 2;  // bytes that are patched (2: near je -> nop;jmp, 1: short je -> jmp)
uint8_t g_gpsOrig[2] = {}, g_gpsNew[2] = {};
std::atomic<bool> g_gpsPatched{false};
std::atomic<int> g_rewrites{0};
std::atomic<bool> g_observeOnly{false};  // count GPS lookups, redirect nothing
std::atomic<int> g_gpsSightings{0};
std::atomic<ULONGLONG> g_lastRedirectTick{0};

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
            g_gpsSightings.fetch_add(1);
            if (g_observeOnly) continue;
            g_lastRedirectTick = GetTickCount64();
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

// The resource manager's "find or load" entry: (manager, out, const char** path, u64* options, u64 flags). It builds the
// cache key "<path>.<options>" and only queues a load if that key is not cached yet. A texture that is already resident
// never reaches the queue, so the redirect also has to happen here: the path is swapped for the duration of the call
// (the caller's own string is not touched), which makes the manager look up and load our placeholder instead.
const char* kLoadSignature =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? F6 84 24 ?? ?? ?? ?? 01 4D 8B E9 "
    "49 8B D8";

using LoadFn = void*(__fastcall*)(void*, void*, const char**, void*, uint64_t);
LoadFn g_origLoad = nullptr;
std::atomic<int> g_loadRedirects{0};

void* __fastcall HookLoad(void* self, void* out, const char** path, void* opts, uint64_t flags) {
    const char* saved = nullptr;
    bool swapped = false;
    if (!g_off && path) {
        __try {
            const char* p = *path;
            if (p) {
                if (strcmp(p, kGameGpsPath) == 0) {
                    g_gpsSightings.fetch_add(1);
                    if (!g_observeOnly) {
                        g_lastRedirectTick = GetTickCount64();
                        saved = p;
                        *path = g_override;
                        swapped = true;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            swapped = false;
        }
    }
    void* result = g_origLoad(self, out, path, opts, flags);
    if (swapped) {
        __try {
            *path = saved;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        const int n = g_loadRedirects.fetch_add(1) + 1;
        if (n <= 5) Logf("ENGINE redirected a lookup of the cabin GPS texture to '%s' (total %d)", g_override, n);
    }
    return result;
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
    // The compiler may emit the jump as a near `je` (0F 84 rel32, game 1.61/1.62) or as a short one (74 rel8): try both.
    uint8_t* gps = FindUniquePattern(exe, kGpsWindowSignature, &hits);
    bool shortJump = false;
    if (!gps) {
        const int nearHits = hits;
        gps = FindUniquePattern(exe, kGpsWindowSignatureShort, &hits);
        shortJump = gps != nullptr;
        if (!gps) hits = nearHits;
    }
    if (gps) {
        // Sanity check: the lea after the jump must point at the string "GPSWindow".
        const int leaDisp = shortJump ? 22 : 26;
        int32_t rel = *reinterpret_cast<int32_t*>(gps + leaDisp);
        const char* name = reinterpret_cast<const char*>(gps + leaDisp + 4 + rel);
        const bool jumpOk = shortJump ? gps[10] == 0x74 : (gps[10] == 0x0F && gps[11] == 0x84);
        if (jumpOk && SafeStringEquals(name, "GPSWindow")) {
            g_gpsJump = gps + 10;
            g_gpsLen = shortJump ? 1 : 2;
            if (shortJump) {  // 74 rel8 (je) -> EB rel8 (jmp)
                g_gpsOrig[0] = 0x74;
                g_gpsNew[0] = 0xEB;
            } else {  // 0F 84 rel32 (je) -> 90 E9 rel32 (nop; jmp)
                g_gpsOrig[0] = 0x0F;
                g_gpsOrig[1] = 0x84;
                g_gpsNew[0] = 0x90;
                g_gpsNew[1] = 0xE9;
            }
            if (shortJump) Logf("ENGINE GPS window jump found in its short form");
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
    uint8_t* load = FindUniquePattern(exe, kLoadSignature, &hits);
    if (load) {
        st = MH_CreateHook(load, reinterpret_cast<void*>(&HookLoad), reinterpret_cast<void**>(&g_origLoad));
        if (st == MH_OK) st = MH_EnableHook(load);
        if (st != MH_OK) {
            Logf("ENGINE load hook failed: %s", MH_StatusToString(st));
            load = nullptr;
        }
    } else {
        Logf("ENGINE resource load signature: %d matches (need exactly 1), only the queue hook is used", hits);
    }
    Logf("ENGINE hooks ready: queue fn at exe+0x%llx, load fn %s, GPS window jump %s",
         static_cast<unsigned long long>(queue - reinterpret_cast<uint8_t*>(exe)), load ? "hooked" : "NOT hooked",
         g_gpsJump ? "found" : "NOT found");
    // Same order as the original approach: the game's GPS UI is off before any truck is loaded.
    if (disableGameGps) EngineDisableGameGps();
    return true;
}

// Writes the (1 or 2) bytes of the GPS window jump.
static bool WriteGpsJump(const uint8_t* bytes) {
    DWORD old = 0;
    if (!VirtualProtect(g_gpsJump, 2, PAGE_EXECUTE_READWRITE, &old)) return false;
    if (g_gpsLen == 2) {
        short word;
        memcpy(&word, bytes, 2);
        _InterlockedExchange16(reinterpret_cast<short*>(g_gpsJump), word);
    } else {
        _InterlockedExchange8(reinterpret_cast<char*>(g_gpsJump), static_cast<char>(bytes[0]));
    }
    VirtualProtect(g_gpsJump, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_gpsJump, 2);
    return true;
}

void EngineDisableGameGps() {
    if (!g_gpsJump || g_gpsPatched.exchange(true)) return;
    if (!WriteGpsJump(g_gpsNew)) {
        Logf("ENGINE VirtualProtect failed (%lu)", GetLastError());
        g_gpsPatched = false;
        return;
    }
    Logf("ENGINE game GPS window switched off");
}

void EngineRestoreGameGps() {
    if (g_gpsJump && g_gpsPatched.exchange(false)) {
        if (WriteGpsJump(g_gpsOrig)) Logf("ENGINE game GPS window switched back on");
    }
}

void EngineHooksDisable() {
    g_off = true;
    EngineRestoreGameGps();
}

void EngineSetObserveOnly(bool on) { g_observeOnly = on; }
int EngineRedirects() { return g_rewrites.load() + g_loadRedirects.load(); }
int EngineGpsSightings() { return g_gpsSightings.load(); }
bool EngineRedirectRecent(unsigned ms) {
    const ULONGLONG t = g_lastRedirectTick.load();
    return t != 0 && GetTickCount64() - t <= ms;
}

}  // namespace ep
