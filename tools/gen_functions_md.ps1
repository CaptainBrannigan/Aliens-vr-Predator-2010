# Regenerates docs/re/FUNCTIONS.md from docs/re/avp_symbols.csv (the source of truth).
# Usage:  & "C:\Dev\VR Mod\AVP2010\main\tools\gen_functions_md.ps1"
# Push the same names into the Ghidra project with tools/ghidra/ApplySymbols.java (writable run, Ghidra closed).
$root = Split-Path $PSScriptRoot -Parent
$rows = Import-Csv (Join-Path $root 'docs\re\avp_symbols.csv')
$order = @('Camera', 'Render', 'HUD', 'Views', 'Look', 'Input', 'Cvar', 'Console', 'Character', 'Misc')
$areas = $order + ($rows.area | Sort-Object -Unique | Where-Object { $order -notcontains $_ })
$funcs = @($rows | Where-Object { $_.kind -eq 'F' }).Count
$labels = @($rows | Where-Object { $_.kind -ne 'F' }).Count
$out = @(
    '# AvP 2010 identified functions and globals'
    ''
    'Generated from `avp_symbols.csv` (the source of truth; edit it, then re-run `tools/gen_functions_md.ps1` and'
    '`tools/ghidra/ApplySymbols.java`).'
    'Program: AvP_DX11.exe, image base 00400000, addresses = x32dbg addresses. Confidence: high = read from decompile/live; medium = inferred; open = unknown.'
    ''
    "$funcs functions, $labels globals/vtables."
)
foreach ($area in $areas) {
    $inArea = $rows | Where-Object { $_.area -eq $area } | Sort-Object { [Convert]::ToUInt32($_.address, 16) }
    if (-not $inArea) { continue }
    $out += ''; $out += "## $area"; $out += ''
    $out += '| Address | Kind | Name | Conf. | Role |'; $out += '|---|---|---|---|---|'
    foreach ($r in $inArea) {
        $kind = if ($r.kind -eq 'F') { 'func' } else { 'data' }
        $out += ('| `{0}` | {1} | `{2}` | {3} | {4} |' -f $r.address.ToUpper(), $kind, $r.name, $r.confidence, $r.role)
    }
}
Set-Content -Path (Join-Path $root 'docs\re\FUNCTIONS.md') -Value $out -Encoding UTF8
"wrote FUNCTIONS.md: $funcs functions, $labels globals"
