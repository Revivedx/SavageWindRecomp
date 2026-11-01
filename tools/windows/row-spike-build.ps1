$ErrorActionPreference = 'Stop'
$repo = 'X:\.Development\Projects\SavageWindRecomp'
$wt = 'X:\.Development\swr-wt\row-spike'
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
$env:PYTHONUTF8 = '1'
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"
git -C $repo fetch origin spike/two-button-row
if ($LASTEXITCODE) { throw 'fetch failed' }
if (-not (Test-Path $wt)) {
    git -C $repo worktree add -b wt/row-spike $wt origin/spike/two-button-row
    if ($LASTEXITCODE) { throw 'worktree creation failed' }
}
if (git -C $wt status --porcelain) { throw 'row-spike checkout has local changes' }
git -C $wt merge --ff-only origin/spike/two-button-row
if ($LASTEXITCODE) { throw 'fast-forward failed' }
New-Item -ItemType Directory "$wt\build" -Force | Out-Null
$log = "$wt\build\row-spike-build.log"
$status = "$wt\build\row-spike-build.status"
Remove-Item $status -ErrorAction SilentlyContinue
$py = '/c/Users/derfa/AppData/Local/Programs/Python/Python312/python.exe'
$timer = Get-Date
& C:\msys64\usr\bin\bash.exe -lc "export PATH=/ucrt64/bin:/usr/bin:`$PATH; cd /x/.Development/swr-wt/row-spike && make test-actions test-bind-capture test-pc-controls test-input-focus test-gyro test-two-button-rows && make -j16 B=build/win-release RELEASE=1 build/win-release/savagewind.exe build/win-release/savagewind-config.exe && make gen-helper GEN_PY='$py' && build/gen-venv/Scripts/python.exe tools/test_game_sites.py && build/gen-venv/Scripts/python.exe tools/test_two_button_sites.py" *> $log
"$LASTEXITCODE after $([int]((Get-Date)-$timer).TotalSeconds)s" | Set-Content $status
if ($LASTEXITCODE) { throw "build/tests failed: $log" }
