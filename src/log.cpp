#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace ep {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
ULONGLONG g_start = 0;
std::wstring g_docsDir;
std::wstring g_pluginDir;

}  // namespace

void SetGameDirs(const std::wstring& docsDir, const std::wstring& pluginDir) {
    g_docsDir = docsDir;
    g_pluginDir = pluginDir;
}

const std::wstring& PluginDir() { return g_pluginDir; }
const std::wstring& GameDocsDir() { return g_docsDir; }

void LogInit(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\diag.log";
    std::wstring prev = dir + L"\\diag.prev.log";
    MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfsopen(path.c_str(), L"wb", _SH_DENYNO);
    g_start = GetTickCount64();
}

void Logf(const char* fmt, ...) {
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n >= static_cast<int>(sizeof(msg))) n = sizeof(msg) - 1;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    ULONGLONG t = GetTickCount64() - g_start;
    fprintf(g_file, "[%02llu:%02llu:%02llu.%03llu] [%5lu] ", t / 3600000, (t / 60000) % 60,
            (t / 1000) % 60, t % 1000, GetCurrentThreadId());
    fwrite(msg, 1, static_cast<size_t>(n), g_file);
    fputc('\n', g_file);
    fflush(g_file);
}

void LogShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

}  // namespace ep
