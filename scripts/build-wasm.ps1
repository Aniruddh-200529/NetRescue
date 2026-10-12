# Compiles the C simulation engine to WebAssembly for the website.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build-wasm.ps1 [-Emsdk C:\path\to\emsdk]
#
# Output: web\src\engine\generated\netrescue_engine.mjs + netrescue_engine.wasm
# The generated files are committed, so the website runs without Emscripten installed;
# re-run this script only after changing the C sources.
param([string]$Emsdk = $env:EMSDK)

$ErrorActionPreference = 'Stop'
if (-not $Emsdk) { $Emsdk = Join-Path $env:USERPROFILE 'emsdk' }
$envScript = Join-Path $Emsdk 'emsdk_env.ps1'
if (-not (Test-Path $envScript)) { throw "Emscripten SDK not found at $Emsdk (pass -Emsdk or set EMSDK)." }
& $envScript | Out-Null

$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'web\src\engine\generated'
New-Item -ItemType Directory -Force $out | Out-Null

$modules = 'network', 'cidr', 'persistence', 'arq', 'topology', 'congestion', 'validate', 'json', 'experiment', 'presets', 'api'
$sources = $modules | ForEach-Object { Join-Path $root "src\$_.c" }

$flags = @(
  '-std=c11', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-I', (Join-Path $root 'include'),
  '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sEXPORT_NAME=createNetRescueEngine',
  '-sENVIRONMENT=web,worker,node', '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=33554432', '-sSTACK_SIZE=2097152',
  '-sEXPORTED_FUNCTIONS=_nr_api_call,_nr_api_shutdown,_malloc,_free',
  '-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString',
  '-o', (Join-Path $out 'netrescue_engine.mjs')
)

Write-Host "Compiling $($sources.Count) C modules to WebAssembly..."
& emcc @sources @flags
if ($LASTEXITCODE -ne 0) { throw "emcc failed with exit code $LASTEXITCODE" }
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize
Write-Host "Done. WebAssembly engine written to $out"
