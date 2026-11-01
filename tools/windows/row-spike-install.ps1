$ErrorActionPreference = 'Stop'
$wt = 'X:\.Development\swr-wt\row-spike'
$src = 'C:\Users\derfa\swr-check\a4-act'
$priorRun = 'C:\Users\derfa\swr-check\act-run\play'
$dst = 'C:\Users\derfa\swr-check\row-spike'
if (Get-Process savagewind -ErrorAction SilentlyContinue | Where-Object Path -EQ "$dst\savagewind.exe") {
    throw 'Close the row-spike game before installing'
}
if (-not ((Get-Content "$wt\build\row-spike-build.status") -match '^0 after')) { throw 'Build did not pass' }
New-Item -ItemType Directory $dst -Force | Out-Null
Get-ChildItem $src -File | Where-Object { $_.Extension -in '.dll', '.ttf', '.txt', '.png' } | Copy-Item -Destination $dst -Force
foreach ($folder in 'zig', 'colour-luts') {
    if (-not (Test-Path "$dst\$folder")) { Copy-Item "$src\$folder" "$dst\$folder" -Recurse }
}
Copy-Item "$wt\build\win-release\savagewind.exe", "$wt\build\win-release\savagewind.dll.a" $dst -Force
Copy-Item "$wt\build\win-release\savagewind-config.exe" "$dst\Savage Wind Configuration.exe" -Force
if (Test-Path "$dst\swgen") { Remove-Item "$dst\swgen" -Recurse -Force }
Copy-Item "$wt\build\helper\swgen" "$dst\swgen" -Recurse
New-Item -ItemType Directory "$dst\gamelib-src", "$dst\extracted" -Force | Out-Null
foreach ($file in 'runtime\cpu.h', 'runtime\simd.h', 'runtime\simd_fp.h', 'runtime\simd_softfma.h', 'runtime\gamelib.h', 'tools\gamelib_entry.c', 'runtime\hle\hle_symbols.txt') {
    Copy-Item "$wt\$file" "$dst\gamelib-src\" -Force
}
# Copy the small run folder once. Later installs preserve this spike's edited binds and save.
if (-not (Test-Path "$dst\savagewind.ini")) {
    Copy-Item "$priorRun\savagewind.ini" $dst
    foreach ($folder in 'save', 'build') { Copy-Item "$priorRun\$folder" "$dst\$folder" -Recurse }
}
# Own exefs and generated code; immutable game resources are shared through directory junctions.
if (-not (Test-Path "$dst\extracted\exefs")) {
    Copy-Item 'X:\.Development\Projects\SavageWindRecomp\extracted\exefs' "$dst\extracted\exefs" -Recurse
}
foreach ($folder in 'romfs', 'aoc1', 'aoc2') {
    if (-not (Test-Path "$dst\extracted\$folder")) {
        New-Item -ItemType Junction -Path "$dst\extracted\$folder" -Target "$src\extracted\$folder" | Out-Null
    }
}
git -C $wt rev-parse HEAD | Set-Content "$dst\installed-commit.txt"
Write-Output "Installed $dst"
