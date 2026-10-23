@echo off
rem Builds EtsNowPlaying.dll with the Visual Studio 2022 Build Tools (cmake + ninja ship with them).
setlocal
set "BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
call "%BT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
echo.
echo Built: %~dp0build\EtsNowPlaying.dll
