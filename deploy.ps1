# Copies the built plugin into the game's plugins folder.
#   .\deploy.ps1                 install EtsNowPlaying.dll
#   .\deploy.ps1 -DisableOld     also move PrismTextureStreamerFB.dll out of the way (kept in plugins\_disabled)
#   .\deploy.ps1 -Uninstall      remove EtsNowPlaying.dll and restore the old plugin if it was moved
param(
    [string]$GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\Euro Truck Simulator 2',
    [switch]$DisableOld,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$plugins = Join-Path $GameDir 'bin\win_x64\plugins'
$disabled = Join-Path $plugins '_disabled'
$dll = Join-Path $PSScriptRoot 'build\EtsNowPlaying.dll'
$old = 'PrismTextureStreamerFB.dll'

if (Get-Process eurotrucks2 -ErrorAction SilentlyContinue) {
    throw 'ETS2 is running. Close the game first, the DLL is loaded at startup.'
}

if ($Uninstall) {
    Remove-Item (Join-Path $plugins 'EtsNowPlaying.dll') -ErrorAction SilentlyContinue
    if (Test-Path (Join-Path $disabled $old)) {
        Move-Item (Join-Path $disabled $old) (Join-Path $plugins $old)
        Write-Host "Restored $old"
    }
    Write-Host 'Uninstalled.'
    return
}

if (-not (Test-Path $dll)) { throw "Not built yet: $dll (run build.bat)" }

if ($DisableOld -and (Test-Path (Join-Path $plugins $old))) {
    New-Item -ItemType Directory -Force $disabled | Out-Null
    Move-Item (Join-Path $plugins $old) (Join-Path $disabled $old) -Force
    Write-Host "Moved $old to $disabled"
} elseif (Test-Path (Join-Path $plugins $old)) {
    Write-Warning "$old is still installed. Both plugins patch the same GPS texture; use -DisableOld while testing."
}

Copy-Item $dll (Join-Path $plugins 'EtsNowPlaying.dll') -Force
Write-Host "Installed EtsNowPlaying.dll to $plugins"

# A new build starts with every feature on again (the crash fallback state belongs to the previous build).
$state = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Euro Truck Simulator 2\EtsNowPlaying\state.ini'
if (Test-Path $state) {
    Remove-Item $state
    Write-Host 'Reset crash fallback state.'
}
