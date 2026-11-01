param([ValidateSet('setup', 'play', 'shot')][string]$Mode = 'play')
$ErrorActionPreference = 'Stop'
$root = 'C:\Users\derfa\swr-check\row-spike'
Start-Transcript -Path "$root\$Mode-task.log" -Force | Out-Null
if ($Mode -eq 'shot') {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bitmap = [System.Drawing.Bitmap]::new($bounds.Width, $bounds.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $bitmap.Save("$root\desktop.png")
    $graphics.Dispose(); $bitmap.Dispose()
    exit
}
if ($Mode -ne 'setup' -and (Get-Process savagewind -ErrorAction SilentlyContinue)) { throw 'A Savage Wind process is already running; left it untouched' }
Get-ChildItem Env:SAVAGEWIND_* | Remove-Item
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"
$env:SAVAGEWIND_DATA_ROOT = $root
$env:SAVAGEWIND_SETTINGS = "$root\savagewind.ini"
$env:SAVAGEWIND_SAVE_DIR = "$root\save"
$env:SAVAGEWIND_LOG_OPTIONS = '1'
$env:SAVAGEWIND_PC_CURSOR_LOG = '1'
$env:SAVAGEWIND_TWO_BUTTON_ROWS = '1'
$start = @{
    FilePath = "$root\savagewind.exe"
    WorkingDirectory = $root
    PassThru = $true
    RedirectStandardOutput = "$root\$Mode-stdout.log"
    RedirectStandardError = "$root\$Mode-stderr.log"
}
if ($Mode -eq 'setup') {
    # Empty source rebuilds code from our copied exefs without copying/deleting game resources.
    $start.Environment = @{ SAVAGEWIND_LAUNCHER_EXTRACT = ''; SAVAGEWIND_HIDDEN = '1' }
}
$p = Start-Process @start
"pid $($p.Id) mode $Mode $(Get-Date -Format s)" | Set-Content "$root\started.txt"
if ($Mode -ne 'play') {
    if (-not $p.WaitForExit(900000)) { throw "Still running pid $($p.Id); see logs" }
    "exit $($p.ExitCode) $(Get-Date -Format s)" | Add-Content "$root\started.txt"
}
