cmake_minimum_required(VERSION 3.20)
project(EtsNowPlaying LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the plugin must not need any redistributable on the target machine.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

add_library(EtsNowPlaying SHARED
    src/plugin.cpp
    src/log.cpp
    src/sigscan.cpp
    src/safe_mode.cpp
    src/engine_hooks.cpp
    src/screen_assets.cpp
    src/d3d11_hooks.cpp
    src/net_hooks.cpp
    src/radio_tracker.cpp
    src/station_db.cpp
    src/media.cpp
    src/settings.cpp
    src/text_util.cpp
    src/http.cpp
    src/cover_lookup.cpp
    src/renderer.cpp
    src/exports.def)
set_target_properties(EtsNowPlaying PROPERTIES PREFIX "")
target_include_directories(EtsNowPlaying PRIVATE src third_party/scssdk/include)
target_compile_definitions(EtsNowPlaying PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(EtsNowPlaying PRIVATE /utf-8 /EHsc /permissive- /W3)
target_link_libraries(EtsNowPlaying PRIVATE
    minhook d3d11 dxgi d2d1 dwrite windowscodecs winhttp ws2_32 windowsapp shell32 ole32)
