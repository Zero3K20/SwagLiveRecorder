@echo off
setlocal

:: build_deps.cmd  –  manually build libdatachannel for all configurations.
::
:: This script is a manual fallback. When you build the solution in Visual
:: Studio 2019, libdatachannel is built automatically via libdatachannel.vcxproj
:: (a Makefile project that invokes CMake). Run this script only if you need to
:: rebuild the dependency outside of Visual Studio.
::
:: Requires CMake 3.13+ on the PATH.

set DEPS_DIR=%~dp0deps\libdatachannel

echo [build_deps] Building libdatachannel x64...
cmake -B "%DEPS_DIR%\build\x64" "%DEPS_DIR%" ^
      -G "Visual Studio 16 2019" -A x64 ^
      -DUSE_MBEDTLS=ON ^
      -DUSE_NICE=OFF ^
      -DNO_TESTS=ON ^
      -DNO_EXAMPLES=ON ^
      -DBUILD_SHARED_LIBS=ON
if errorlevel 1 ( echo ERROR: CMake configure (x64) failed. & exit /b 1 )

cmake --build "%DEPS_DIR%\build\x64" --config Release --parallel
if errorlevel 1 ( echo ERROR: Release x64 build failed. & exit /b 1 )

cmake --build "%DEPS_DIR%\build\x64" --config Debug --parallel
if errorlevel 1 ( echo ERROR: Debug x64 build failed. & exit /b 1 )

echo [build_deps] Building libdatachannel Win32...
cmake -B "%DEPS_DIR%\build\Win32" "%DEPS_DIR%" ^
      -G "Visual Studio 16 2019" -A Win32 ^
      -DUSE_MBEDTLS=ON ^
      -DUSE_NICE=OFF ^
      -DNO_TESTS=ON ^
      -DNO_EXAMPLES=ON ^
      -DBUILD_SHARED_LIBS=ON
if errorlevel 1 ( echo ERROR: CMake configure (Win32) failed. & exit /b 1 )

cmake --build "%DEPS_DIR%\build\Win32" --config Release --parallel
if errorlevel 1 ( echo ERROR: Release Win32 build failed. & exit /b 1 )

cmake --build "%DEPS_DIR%\build\Win32" --config Debug --parallel
if errorlevel 1 ( echo ERROR: Debug Win32 build failed. & exit /b 1 )

echo [build_deps] Done.
endlocal

