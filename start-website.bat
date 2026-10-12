@echo off
rem Installs the website dependencies on first run and starts it at http://localhost:5173
rem The C engine is already compiled to WebAssembly (web\src\engine\generated); Emscripten is only
rem needed after changing the C sources (scripts\build-wasm.ps1).
setlocal
cd /d "%~dp0web"
where npm >nul 2>nul || (echo Node.js is required: https://nodejs.org ^(LTS^) & exit /b 1)
if not exist node_modules (
  echo Installing website dependencies...
  call npm install || exit /b 1
)
call npm run dev -- --open
