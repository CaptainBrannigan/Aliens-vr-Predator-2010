# Copies the built proxy into the game folder (or removes it with -Remove).
# Usage:  & "C:\Dev\VR Mod\AVP2010\main\tools\deploy.ps1" [-Configuration Release|Debug] [-Remove]
param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$Remove,
    [string]$GameDir = 'E:\SteamLibrary\steamapps\common\Aliens vs Predator'
)
$target = Join-Path $GameDir 'dxgi.dll'
if ($Remove) {
    if (Test-Path $target) { Remove-Item $target; "removed $target" } else { "nothing to remove at $target" }
    return
}
$built = Join-Path $PSScriptRoot "..\bin\$Configuration\dxgi.dll"
if (-not (Test-Path $built)) { throw "not built: $built" }
Copy-Item $built $target -Force
"deployed $Configuration build -> $target"
"logs will appear next to it: avpvr_main.log, avpvr_trace.log, avpvr_snapshot.log"
