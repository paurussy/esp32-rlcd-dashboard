<#
  install_tasks.ps1 - Installs the PC programs as scheduled tasks

    "RLCD Teams calendar bridge"  calendar_bridge --once, hidden, every 15 minutes just
                                  before the board switches its WiFi on (:59, :14, :29,
                                  :44). Only while you are signed in (Outlook needs your
                                  session).
    "RLCD tasks server"           tasks_server, started at sign-in and kept running. Only
                                  if config.ini has a [tasks] token.

  Usage:   powershell -ExecutionPolicy Bypass -File install_tasks.ps1
  Remove:  Unregister-ScheduledTask -TaskName "RLCD Teams calendar bridge"
           Unregister-ScheduledTask -TaskName "RLCD tasks server"
#>
$ErrorActionPreference = "Stop"

$calendarTaskName = "RLCD Teams calendar bridge"
$tasksTaskName = "RLCD tasks server"

# Use the pythonw.exe of the same interpreter as "python" (the one pip installed
# the requirements into), not whichever pythonw comes first on PATH
$python = (Get-Command python.exe).Source
$pythonw = & $python -c "import sys, pathlib; print(pathlib.Path(sys.executable).with_name('pythonw.exe'))"
if (-not (Test-Path $pythonw)) { $pythonw = (Get-Command pythonw.exe).Source }
& $python -c "import ccl_chromium_reader" 2>$null
if ($LASTEXITCODE -ne 0) {
  throw "Dependencies missing for $python : run python -m pip install -r requirements.txt"
}

$configPath = Join-Path $PSScriptRoot "config.ini"
if (-not (Test-Path $configPath)) {
  throw "config.ini not found: copy config.example.ini and edit it first."
}

# ---------------- Calendar bridge ----------------
# The programs are Python packages, run with -m from this folder
$action = New-ScheduledTaskAction -Execute $pythonw -Argument "-m calendar_bridge --once" -WorkingDirectory $PSScriptRoot
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

Register-ScheduledTask -TaskName $calendarTaskName -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Write-Host "Installed '$calendarTaskName': every 15 min at :13:40, :28:40, :43:40, :58:40 ($pythonw -m calendar_bridge --once)"
$next = (Get-ScheduledTaskInfo -TaskName $calendarTaskName).NextRunTime
Write-Host "Next run: $($next.ToString('HH:mm:ss')). Run it now with: Start-ScheduledTask -TaskName '$calendarTaskName'"

# ---------------- Tasks server ----------------
# Only with [tasks] token in config.ini (the board ignores tasks otherwise); a task left
# from an earlier install is removed.
$tasksToken = & $python -c "import configparser, sys; c = configparser.ConfigParser(); c.read(sys.argv[1], encoding='utf-8'); print(c.get('tasks', 'token', fallback='').strip())" $configPath
if ($tasksToken) {
  $tasksAction = New-ScheduledTaskAction -Execute $pythonw -Argument "-m tasks_server" -WorkingDirectory $PSScriptRoot
  $tasksTrigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
  # No time limit; restarted if it ever dies
  $tasksSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
  Register-ScheduledTask -TaskName $tasksTaskName -Action $tasksAction -Trigger $tasksTrigger -Settings $tasksSettings -Force | Out-Null
  Start-ScheduledTask -TaskName $tasksTaskName
  Write-Host "Installed '$tasksTaskName': starts at sign-in and is running now (http://localhost:8765, $pythonw -m tasks_server)"
} elseif (Get-ScheduledTask -TaskName $tasksTaskName -ErrorAction SilentlyContinue) {
  Unregister-ScheduledTask -TaskName $tasksTaskName -Confirm:$false
  Write-Host "Removed '$tasksTaskName': no [tasks] token in config.ini"
}
