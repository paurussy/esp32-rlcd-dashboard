<#
  install_tasks.ps1 - Installs the PC programs as scheduled tasks

    "RLCD Teams calendar bridge"  calendar_bridge --once, hidden, every 15 minutes just
                                  before the board switches its WiFi on (:59, :14, :29,
                                  :44). Only while you are signed in (Outlook needs your
                                  session). Every minute, at any hour, when the board's
                                  power saving is off ([esp32] power_saving = false).
    "RLCD tasks server"           tasks_server, started at sign-in and kept running. Only
                                  if config.ini has a [tasks] token. With power saving off
                                  it starts with a full sync of the tasks.
    "RLCD calendar full sync"     Only with power saving off: calendar_bridge --once
                                  --resync at sign-in, so a board switched on with the PC
                                  gets the calendar even if it did not change.

  Usage:   powershell -ExecutionPolicy Bypass -File install_tasks.ps1
  Remove:  Unregister-ScheduledTask -TaskName "RLCD Teams calendar bridge"
           Unregister-ScheduledTask -TaskName "RLCD calendar full sync"
           Unregister-ScheduledTask -TaskName "RLCD tasks server"
#>
$ErrorActionPreference = "Stop"

$calendarTaskName = "RLCD Teams calendar bridge"
$tasksTaskName = "RLCD tasks server"
$resyncTaskName = "RLCD calendar full sync"

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

# [esp32] power_saving in config.ini (default true): must match POWER_SAVING in config.h
$powerSaving = & $python -c "import configparser, sys; c = configparser.ConfigParser(); c.read(sys.argv[1], encoding='utf-8'); print(c.getboolean('esp32', 'power_saving', fallback=True))" $configPath
$powerSaving = $powerSaving -eq "True"

# ---------------- Calendar bridge ----------------
# The programs are Python packages, run with -m from this folder
$action = New-ScheduledTaskAction -Execute $pythonw -Argument "-m calendar_bridge --once" -WorkingDirectory $PSScriptRoot
if ($powerSaving) {
  # The board's WiFi window opens at :59, :14, :29, :44 (WIFI_WINDOW_* in config.h) and the
  # Outlook read takes ~20 s: start at :58:40 on the same 15 min grid. Outside the board's active
  # hours (08:00-17:00) the program exits at once without reading Outlook. No sign-in trigger and no
  # StartWhenAvailable: a late run would fall outside the window.
  # The start is a fixed point on that grid (midnight + 13:40), not the time of this install: the
  # repetition keeps counting from it, so runs always land on :13:40, :28:40, :43:40 and :58:40.
  $start = (Get-Date).Date.AddMinutes(13).AddSeconds(40)
  # No RepetitionDuration: repeats indefinitely
  $trigger = New-ScheduledTaskTrigger -Once -At $start -RepetitionInterval (New-TimeSpan -Minutes 15)
  $timeLimit = New-TimeSpan -Minutes 3
  $schedule = "every 15 min at :13:40, :28:40, :43:40, :58:40"
} else {
  # Power saving off: the board has WiFi on 24/7, so push every minute at any hour. A run
  # still going (slow Outlook read) makes the next one be skipped (IgnoreNew).
  $trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).Date -RepetitionInterval (New-TimeSpan -Minutes 1)
  $timeLimit = New-TimeSpan -Minutes 2
  $schedule = "every minute, at any hour (power saving off)"
}
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
  -MultipleInstances IgnoreNew -ExecutionTimeLimit $timeLimit

Register-ScheduledTask -TaskName $calendarTaskName -Action $action -Trigger $trigger -Settings $settings -Force | Out-Null
Write-Host "Installed '$calendarTaskName': $schedule ($pythonw -m calendar_bridge --once)"
$next = (Get-ScheduledTaskInfo -TaskName $calendarTaskName).NextRunTime
Write-Host "Next run: $($next.ToString('HH:mm:ss')). Run it now with: Start-ScheduledTask -TaskName '$calendarTaskName'"

# ---------------- Calendar full sync at sign-in ----------------
# Power saving off: the calendar is only pushed when it changes, but a board switched on
# with the PC has nothing. At sign-in push it in full; until the board answers the
# every-minute task keeps trying.
if (-not $powerSaving) {
  $resyncAction = New-ScheduledTaskAction -Execute $pythonw -Argument "-m calendar_bridge --once --resync" -WorkingDirectory $PSScriptRoot
  $resyncTrigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
  $resyncSettings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 3)
  Register-ScheduledTask -TaskName $resyncTaskName -Action $resyncAction -Trigger $resyncTrigger -Settings $resyncSettings -Force | Out-Null
  Write-Host "Installed '$resyncTaskName': at sign-in ($pythonw -m calendar_bridge --once --resync)"
} elseif (Get-ScheduledTask -TaskName $resyncTaskName -ErrorAction SilentlyContinue) {
  Unregister-ScheduledTask -TaskName $resyncTaskName -Confirm:$false
  Write-Host "Removed '$resyncTaskName': power saving is on"
}

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
