# Copies the built proxy into the game folder (or removes it with -Remove), plus a 32-bit openvr_api.dll if the game
# folder doesn't have one yet (loaded dynamically by vr/openvr_session.cpp; never linked).
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

$openvr = Join-Path $GameDir 'openvr_api.dll'
if (-not (Test-Path $openvr)) {
    # The MOHW mod's copy (proven with SteamVR's 32-bit runtime), else SteamVR's own win32 copy.
    $sources = @('C:\Games\Medal of Honor Warfighter\openvr_api.dll', 'C:\Steam\steamapps\common\SteamVR\bin\win32\openvr_api.dll')
    $src = $sources | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($src) { Copy-Item $src $openvr; "copied 32-bit openvr_api.dll from $src" } else { "WARNING: no 32-bit openvr_api.dll found -- VR will stay off" }
}
"logs will appear next to it: avpvr_main.log, avpvr_vr.log, avpvr_stereo.log, avpvr_trace.log, avpvr_snapshot.log"
