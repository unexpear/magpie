# Register the Magpie background runner with Windows Task Scheduler.
#
# Task Scheduler rather than a resident tray app: it survives reboots, there is
# no service to install, and the OS already solved "run this every so often".
# The runner itself is stateless between ticks — all state lives in state.json.
#
#   .\install-task.ps1              register (every 2 hours)
#   .\install-task.ps1 -Remove      unregister
#   .\install-task.ps1 -Every 60    a different cadence, in minutes

param(
    [switch]$Remove,
    [int]$Every = 120
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$name = 'MagpieBackgroundRunner'

if ($Remove) {
    Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "removed scheduled task '$name'"
    return
}

$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { throw 'python not found on PATH' }

$runner = Join-Path $here 'runner.py'
if (-not (Test-Path $runner)) { throw "missing $runner" }

$action = New-ScheduledTaskAction -Execute $python `
            -Argument "`"$runner`"" -WorkingDirectory $here

# Repeat indefinitely from the next round hour.
$trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).Date.AddHours((Get-Date).Hour + 1) `
             -RepetitionInterval (New-TimeSpan -Minutes $Every)

# Settings that keep an unattended job from becoming a nuisance:
#  - only on mains power, and stop if the machine goes onto battery
#  - do not start a second copy if one is somehow still going
#  - give up rather than pile up if a tick overruns badly
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries:$false `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Hours 2) `
    -StartWhenAvailable

Register-ScheduledTask -TaskName $name -Action $action -Trigger $trigger `
    -Settings $settings -Description 'Magpie: retire the uncreditable backlog, politely.' `
    -Force | Out-Null

Write-Host "registered '$name', every $Every minutes"
Write-Host ""
Write-Host "  status page :  $here\status.html"
Write-Host "  stop        :  $here\STOP.cmd     (takes effect before the next request)"
Write-Host "  log         :  $here\runner.log"
Write-Host ""
Write-Host "It will not run during quiet hours (09:00-18:00) and stops at 500 requests/day."
