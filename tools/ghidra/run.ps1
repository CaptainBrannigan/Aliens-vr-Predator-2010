# Runs a Ghidra script headless, read-only, against AvP_DX11.exe in the AVP2010 project.
# Usage: & "C:\Dev\VR Mod\AVP2010\main\tools\ghidra\run.ps1" DecompileList.java outname 006ace60 006ac1e0
# Script output goes to C:\Dev\VR Mod\AVP2010\analysis\ (raw, unversioned). ApplySymbols.java needs a writable
# run instead (no -readOnly) -- see its header.
param([Parameter(Mandatory)][string]$Script, [Parameter(ValueFromRemainingArguments)][string[]]$ScriptArgs)
$ghidra = "C:\Dev\VR Mod\Tools\ghidra_12.1.2_PUBLIC\support\analyzeHeadless.bat"
$log = "C:\Dev\VR Mod\AVP2010\analysis\headless_last.log"
& $ghidra "C:\Dev\VR Mod\AVP2010\GhidraProjectFiles" AVP2010 -process "AvP_DX11.exe" -noanalysis -readOnly `
    -scriptPath $PSScriptRoot -postScript $Script @ScriptArgs *> $log
Select-String $log -Pattern 'Wrote|ERROR|Exception|LockException' | ForEach-Object Line
