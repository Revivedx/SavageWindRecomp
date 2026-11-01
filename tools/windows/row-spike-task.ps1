param([ValidateSet('build', 'setup', 'play', 'shot')][string]$Mode = 'play')
$ErrorActionPreference = 'Stop'
$base = 'C:\Users\derfa'
$script = if ($Mode -eq 'build') { "$base\row-spike-build.ps1" } else { "$base\row-spike-run.ps1" }
$argsForTask = "-NoProfile -File $script"
if ($Mode -ne 'build') { $argsForTask += " -Mode $Mode" }
$action = New-ScheduledTaskAction -Execute (Get-Command pwsh).Source -Argument $argsForTask
$principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().User.Value) -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Hours 2) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName "SWR-RowSpike-$Mode" -Action $action -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName "SWR-RowSpike-$Mode"
