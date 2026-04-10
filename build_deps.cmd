@echo off
setlocal

:: build_deps.cmd  –  build libdatachannel for all configurations.
::
:: Run this once from the repository root before opening the Visual Studio
:: solution.  Requires CMake 3.13+ and Visual Studio 2019 (or later) to be
:: on the PATH.
::
:: libdatachannel (and its dependencies plog, libjuice, libsrtp, usrsctp,
:: nlohmann/json) are included directly in deps\libdatachannel\ — no
:: internet access or git submodule step is needed.
::
:: The output files placed in deps\libdatachannel\build\Release\ and
:: deps\libdatachannel\build\Debug\ are then picked up automatically by the
:: Visual Studio project.

set DEPS_DIR=%~dp0deps\libdatachannel
set BUILD_DIR=%DEPS_DIR%\build

echo [build_deps] Configuring libdatachannel with CMake...
cmake -B "%BUILD_DIR%" "%DEPS_DIR%" ^
      -G "Visual Studio 16 2019" -A x64 ^
      -DUSE_MBEDTLS=ON ^
      -DUSE_NICE=OFF ^
      -DNO_TESTS=ON ^
      -DNO_EXAMPLES=ON ^
      -DBUILD_SHARED_LIBS=ON
if errorlevel 1 (
    echo ERROR: CMake configuration failed.
    exit /b 1
)

echo [build_deps] Building Release...
cmake --build "%BUILD_DIR%" --config Release --parallel
if errorlevel 1 (
    echo ERROR: Release build failed.
    exit /b 1
)

echo [build_deps] Building Debug...
cmake --build "%BUILD_DIR%" --config Debug --parallel
if errorlevel 1 (
    echo ERROR: Debug build failed.
    exit /b 1
)

echo [build_deps] Done.  libdatachannel is ready at:
echo   %BUILD_DIR%\Release\datachannel.lib
echo   %BUILD_DIR%\Release\datachannel.dll
echo.
echo You can now open SwagLiveRecorder.sln in Visual Studio.
endlocal
