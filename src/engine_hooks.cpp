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

// Diagnosis for game updates: which paths pass through the queue? (a few, and everything that mentions "gps")
struct SeenPath {
    char text[200];
};
thread_local SeenPath g_pending[6];
thread_local int g_pendingCount = 0;
uint64_t g_seenHashes[256];
int g_seenCount = 0;
std::atomic<int> g_requestsSeen{0};

bool Interesting(const char* path) {
    char lower[200];
    size_t i = 0;
    for (; path[i] && i < sizeof(lower) - 1; ++i) lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(path[i])));
    lower[i] = 0;
    return strstr(lower, "gps") != nullptr;
}

// Called with the queue lock held: only remembers, the logging happens after the lock is released.
void NotePath(const char* path) {
    const int seen = g_requestsSeen.fetch_add(1);
    uint64_t h = 1469598103934665603ull;
    for (const char* p = path; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 1099511628211ull;
    }
    if (!(seen < 5 || Interesting(path))) return;
    for (int i = 0; i < g_seenCount; ++i) {
        if (g_seenHashes[i] == h) return;
    }
    if (g_seenCount >= 256 || g_pendingCount >= 6) return;
    g_seenHashes[g_seenCount++] = h;
    strncpy_s(g_pending[g_pendingCount].text, path, _TRUNCATE);
    ++g_pendingCount;
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
            if (path) NotePath(path);
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
            g_pendingCount = 0;
            int n = RewriteQueued(static_cast<uint8_t*>(self));
            _InterlockedExchange8(const_cast<char*>(lock), 0);
            for (int i = 0; i < g_pendingCount; ++i) Logf("ENGINE queue: texture request '%s'", g_pending[i].text);
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
std::atomic<int> g_loadCalls{0};

void* __fastcall HookLoad(void* self, void* out, const char** path, void* opts, uint64_t flags) {
    const char* saved = nullptr;
    bool swapped = false;
    if (!g_off && path) {
        char shown[160] = {};
        __try {
            const char* p = *path;
            if (p) {
                if (g_loadCalls.fetch_add(1) < 3) strncpy_s(shown, p, _TRUNCATE);
                if (strcmp(p, kGameGpsPath) == 0) {
                    saved = p;
                    *path = g_override;
                    swapped = true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            swapped = false;
        }
        if (shown[0]) Logf("ENGINE load request seen: '%s'", shown);
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
