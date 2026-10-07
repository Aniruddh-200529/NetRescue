@echo off
setlocal EnableDelayedExpansion
if exist ".netrescue-tools\cmake\data\bin\cmake.exe" (
  set "CMAKE_EXE=%CD%\.netrescue-tools\cmake\data\bin\cmake.exe"
  set "CTEST_EXE=%CD%\.netrescue-tools\cmake\data\bin\ctest.exe"
  "!CMAKE_EXE!" -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="!CD!\.netrescue-tools\bin\ninja.exe" -DCMAKE_C_COMPILER="!CD!\.netrescue-tools\ziglang\zig.exe" -DCMAKE_C_COMPILER_ARG1=cc -DCMAKE_BUILD_TYPE=Release
) else (
  set "CMAKE_EXE=cmake"
  set "CTEST_EXE=ctest"
  "!CMAKE_EXE!" -S . -B build
)
if errorlevel 1 exit /b %errorlevel%
"%CMAKE_EXE%" --build build --config Release --parallel
if errorlevel 1 exit /b %errorlevel%
"%CTEST_EXE%" --test-dir build --output-on-failure
