<#
  install_task.ps1 - Pushes the calendar in every WiFi window of the board

  Runs "teams_bridge.py --once" hidden with pythonw, only while you are signed
  in (Outlook needs your session), every 15 minutes just before the board
  switches its WiFi on (:59, :14, :29, :44).

  Usage:   powershell -ExecutionPolicy Bypass -File install_task.ps1
  Remove:  Unregister-ScheduledTask -TaskName "RLCD Teams calendar bridge"
#>
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
# The board's WiFi window opens at :59, :14, :29, :44 (WIFI_WINDOW_* in config.h) and the
# Outlook read takes ~20 s: start at :58:40 on the same 15 min grid. No sign-in trigger and no
# StartWhenAvailable: a late run would fall outside the window.
# The start is a fixed point on that grid (midnight + 13:40), not the time of this install: the
# repetition keeps counting from it, so runs always land on :13:40, :28:40, :43:40 and :58:40.
$start = (Get-Date).Date.AddMinutes(13).AddSeconds(40)
# No RepetitionDuration: repeats indefinitely
$trigger = New-ScheduledTaskTrigger -Once -At $start -RepetitionInterval (New-TimeSpan -Minutes 15)
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
  -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 3)

Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Write-Host "Installed '$taskName': every 15 min at :13:40, :28:40, :43:40, :58:40 ($pythonw $script --once)"
$next = (Get-ScheduledTaskInfo -TaskName $taskName).NextRunTime
Write-Host "Next run: $($next.ToString('HH:mm:ss')). Run it now with: Start-ScheduledTask -TaskName '$taskName'"
