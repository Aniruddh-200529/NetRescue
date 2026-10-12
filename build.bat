@echo off
rem Builds the native C engine, CLI tools and tests, then runs the C test suite.
rem Uses MSVC if available (Developer prompt), otherwise MinGW-w64 gcc + mingw32-make.
setlocal
where cl >nul 2>nul
if %errorlevel%==0 (
  cmake -S . -B build
) else (
  where gcc >nul 2>nul || (echo No C compiler found. Install Visual Studio Build Tools or MinGW-w64. & exit /b 1)
  cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
)
if errorlevel 1 exit /b %errorlevel%
cmake --build build --config Release --parallel
if errorlevel 1 exit /b %errorlevel%
ctest --test-dir build -C Release --output-on-failure
