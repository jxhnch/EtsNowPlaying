#pragma once
#include "common.h"

namespace ep {

// Opens <dir>\diag.log (the previous run is kept as diag.prev.log).
void LogInit(const std::wstring& dir);
void Logf(const char* fmt, ...);
void LogShutdown();

// Directory the plugin keeps its own files in.
const std::wstring& PluginDir();
// The game's documents folder (contains live_streams.sii, config.cfg, ...).
const std::wstring& GameDocsDir();
void SetGameDirs(const std::wstring& docsDir, const std::wstring& pluginDir);

}  // namespace ep
