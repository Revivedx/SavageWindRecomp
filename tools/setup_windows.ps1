# One-time setup of a Windows machine for building Savage Wind (see docs/BUILD-windows.md).
# Installs MSYS2 (clang, lld, make, SDL3, FFmpeg, Vulkan loader/headers, shaderc) and Python 3.12, then creates
# tools/venv (lifter: unicorn, capstone, lz4). Safe to re-run.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

if (-not (Test-Path C:\msys64\usr\bin\pacman.exe)) {
    winget install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements --silent
}
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
$bash = 'C:\msys64\usr\bin\bash.exe'
& $bash -lc 'pacman -Syuu --noconfirm' | Out-Null
& $bash -lc 'pacman -Syuu --noconfirm' | Out-Null  # second pass after a core update
$pkgs = 'make', 'mingw-w64-ucrt-x86_64-clang', 'mingw-w64-ucrt-x86_64-lld', 'mingw-w64-ucrt-x86_64-sdl3',
        'mingw-w64-ucrt-x86_64-ffmpeg', 'mingw-w64-ucrt-x86_64-vulkan-headers', 'mingw-w64-ucrt-x86_64-vulkan-loader',
        'mingw-w64-ucrt-x86_64-shaderc',
        'mingw-w64-ucrt-x86_64-spirv-tools', 'mingw-w64-ucrt-x86_64-pkgconf', 'mingw-w64-ucrt-x86_64-gdb',
        'mingw-w64-ucrt-x86_64-vulkan-memory-allocator', 'mingw-w64-ucrt-x86_64-vulkan-validation-layers',
        'mingw-w64-ucrt-x86_64-nasm'  # (make deps-windows: FFmpeg's VP9 assembly)
& $bash -lc "pacman -S --needed --noconfirm $($pkgs -join ' ')"

# Python for the tools: the python.org build (prebuilt unicorn / capstone wheels; MSYS2's Python can't build them)
$py = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
if (-not (Test-Path $py)) {
    winget install --id Python.Python.3.12 -e --scope user --accept-source-agreements --accept-package-agreements --silent
}
if (-not (Test-Path "$root\tools\venv\Scripts\python.exe")) { & $py -m venv "$root\tools\venv" }
& "$root\tools\venv\Scripts\python.exe" -m pip install -q lz4 capstone unicorn oead

# LF line endings in this clone (the bash tools break with CRLF; Git for Windows defaults to autocrlf=true)
git -C $root config core.autocrlf false
git -C $root config core.eol lf
Write-Host 'Done. Build from an MSYS2 UCRT64 shell (or put C:\msys64\ucrt64\bin;C:\msys64\usr\bin first on PATH): make -j'
