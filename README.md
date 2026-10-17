# Aliens vs Predator (2010) VR

A VR mod for **Aliens vs Predator (2010)**, DX11 executable. Early framework stage — see `docs/ROADMAP.md`.

## Layout

| Path | Contents |
|---|---|
| `src/proxy` | `dxgi.dll` proxy entry point and exports |
| `src/core` | Logger, build detection, addresses (`addresses.h`), developer flags |
| `src/hooks` | Trace hooks (log-only, generic stub), game-state snapshot, experiments |
| `docs/re` | Reverse-engineering notes: `FINDINGS.md`, `FUNCTIONS.md`, `TODO_AND_DECISIONS.md`, `avp_symbols.csv` |
| `tools/ghidra` | Headless Ghidra scripts (`run.ps1` wrapper, `ApplySymbols.java` pushes the CSV names into the project) |
| `tools/deploy.ps1` | Copies the built `dxgi.dll` into the game folder (`-Remove` takes it out) |
| `reference/mohw_port` | Uncompiled copies of the MOHW mod's VR modules, to be ported |
| `third_party` | MinHook, OpenVR headers |

## Build

Visual Studio 2022 (v143), Win32:

    MSBuild AvPVR.sln /p:Configuration=Release /p:Platform=Win32

Output: `bin\Release\dxgi.dll`. Deploy with `tools\deploy.ps1`; logs (`avpvr_*.log`) appear in the game folder.
