# Undo the Vencord injection and restore Discord's original app.asar.
#
# The old Vencord installer renamed Discord's real app.asar to _app.asar and
# replaced it with a folder whose index.js requires a patcher from this
# project. Deleting the Vencord checkout therefore broke Discord's startup.
#
# This puts the original back. It touches nothing but the two generated stub
# folders and the rename. Run it, then start Discord normally.
#
# Usage: right-click this file -> Run with PowerShell
#        (or: powershell -ExecutionPolicy Bypass -File fix-discord.ps1)

$ErrorActionPreference = 'Stop'

Write-Host ''
Write-Host 'Restoring Discord' -ForegroundColor Cyan
Write-Host ''

# Every Discord build gets its own app-<version> folder, and an update since
# the injection would leave more than one. Fix each that still looks patched.
$roots = Get-ChildItem "$env:LOCALAPPDATA\Discord" -Directory -Filter 'app-*' -ErrorAction SilentlyContinue
if (-not $roots) {
    Write-Host 'No Discord installation found. Nothing to do.' -ForegroundColor Yellow
    Read-Host 'Press Enter to close'
    exit 0
}

$running = Get-Process Discord -ErrorAction SilentlyContinue
if ($running) {
    Write-Host 'Closing Discord...'
    $running | Stop-Process -Force
    Start-Sleep -Seconds 3
}

$fixed = 0
foreach ($root in $roots) {
    $res = Join-Path $root.FullName 'resources'
    $orig = Join-Path $res '_app.asar'
    $live = Join-Path $res 'app.asar'

    if (-not (Test-Path $orig)) { continue }          # not injected
    if (Test-Path -Path $live -PathType Leaf) { continue }  # already restored

    Write-Host "  $($root.Name)"

    # Order matters: clear the stubs before the rename can take the name back.
    foreach ($stub in @((Join-Path $res 'app'), $live)) {
        if (Test-Path $stub) {
            Remove-Item $stub -Recurse -Force
            Write-Host "    removed stub $(Split-Path $stub -Leaf)"
        }
    }

    Rename-Item $orig 'app.asar'
    Write-Host '    restored original app.asar' -ForegroundColor Green
    $fixed++
}

Write-Host ''
if ($fixed -gt 0) {
    Write-Host "Done. $fixed installation(s) restored." -ForegroundColor Green
    Write-Host 'Start Discord normally -- the error is gone and Vencord is removed.'
    Write-Host ''
    Write-Host 'You can now delete the Vencord folder in this project.'
} else {
    Write-Host 'Nothing needed fixing. Discord is already clean.' -ForegroundColor Yellow
}
Write-Host ''
Read-Host 'Press Enter to close'
