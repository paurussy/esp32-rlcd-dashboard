<#
  install_task.ps1 - Pushes the calendar every 15 minutes (and at sign-in)

  Runs "teams_bridge.py --once" hidden with pythonw, only while you are signed
  in (Outlook COM needs your session).

  Usage:   powershell -ExecutionPolicy Bypass -File install_task.ps1 [-IntervalMinutes 15]
  Remove:  Unregister-ScheduledTask -TaskName "RLCD Teams calendar bridge"
#>
param(
  [int]$IntervalMinutes = 15
)
$ErrorActionPreference = "Stop"

$taskName = "RLCD Teams calendar bridge"
$script = Join-Path $PSScriptRoot "teams_bridge.py"

# Use the pythonw.exe of the same interpreter as "python" (the one pip installed
# the requirements into), not whichever pythonw comes first on PATH
$python = (Get-Command python.exe).Source
$pythonw = & $python -c "import sys, pathlib; print(pathlib.Path(sys.executable).with_name('pythonw.exe'))"
if (-not (Test-Path $pythonw)) { $pythonw = (Get-Command pythonw.exe).Source }
& $python -c "import ccl_chromium_reader" 2>$null
if ($LASTEXITCODE -ne 0) {
  throw "Dependencies missing for $python : run python -m pip install -r requirements.txt"
}

if (-not (Test-Path (Join-Path $PSScriptRoot "config.ini"))) {
  throw "config.ini not found: copy config.example.ini and edit it first."
}

$action = New-ScheduledTaskAction -Execute $pythonw -Argument "`"$script`" --once" -WorkingDirectory $PSScriptRoot
$me = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name  # DOMAIN\user, as Task Scheduler wants it
$triggers = @(
  New-ScheduledTaskTrigger -AtLogOn -User $me
  # No RepetitionDuration: repeats indefinitely
  New-ScheduledTaskTrigger -Once -At (Get-Date) -RepetitionInterval (New-TimeSpan -Minutes $IntervalMinutes)
)
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
  -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 5)

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $triggers -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Write-Host "Installed '$taskName': every $IntervalMinutes min and at sign-in ($pythonw $script --once)"
