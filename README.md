# Aliens vs Predator (2010) VR

A VR mod for **Aliens vs Predator (2010)**, the DX11 executable (`AvP_DX11.exe`, Steam build), through in-process
32-bit SteamVR. Loaded as a `dxgi.dll` proxy from the game folder. Progress and plans: `docs/ROADMAP.md`.

## Status: v0.1 (2026-10-17)

**Head look and stereo are complete** for all three species, single player and multiplayer (multiplayer not yet
tested with a second player). Verified in the headset:
- **Stereo**: the game renders twice per frame (left eye, then the right eye from the Present hook), each eye with
  its own asymmetric frustum; full 3D world, lighting, culling.
- **HUD** drawn into a centred panel in both eyes (size, distance, height adjustable); the marine motion tracker's
  flat-screen duplicate hidden.
- **Menus, pause menu, cutscenes, movies, loading screens** on a flat virtual screen in front of you.
- **SteamVR dashboard tab "AvP VR"** with every mod setting (saved to `avpvr_settings.ini`, written on each change).
- **Touch controllers as a virtual Xbox pad** (Quest 3 tested), rebindable in SteamVR's binding UI.
- Level reloads within a session.

- **Head aim** for all three species: the head aims (shots follow it), the right stick turns; the view is the game's
  camera plus the head's difference, upright on Alien walls and ceilings; per-species aim offsets.

Known issue: the **multiplayer pause menu** renders in the eyes, cut off (multiplayer doesn't stop time, so it isn't
detected as a pause; its state is logged for a fix).

Not done yet: motion controls (controller aiming, weapon/hands on the controllers, melee gestures), positional
(6DoF) head tracking, body IK.

## Using it

1. Build (below) or take a `dxgi.dll`, and run `tools\deploy.ps1`: it copies `dxgi.dll` and a 32-bit
   `openvr_api.dll` into the game folder (`-Remove` takes the mod out).
2. Start SteamVR, then the game. **Turn the game's motion blur off** (it smears the second eye).
3. Settings: SteamVR dashboard -> **AvP VR** tab. "Recenter view" is at the top.

Default Touch layout (change it in SteamVR -> Settings -> Controllers -> Manage controller bindings -> AvP):

| Touch | Xbox pad |
|---|---|
| Sticks, stick clicks | Sticks, stick clicks |
| Triggers | LT / RT (analog) |
| A / B / X / Y | A / B / X / Y (held as long as you hold them) |
| Thumbrest touch (left / right) | LB / RB |
| Double press X / Y / B / A | D-pad left / up / right / down |
| Hold the right thumbrest (0.8 s) | Start (RB is held meanwhile) |
| Grips | Reserved for melee gestures (nothing yet); Back unbound |

If you've ever saved your own bindings for the game, SteamVR keeps using that copy: pick "AvP VR defaults" there to
get new actions.

Logs (`avpvr_*.log`) are written next to the game; each launch keeps the previous one as `*.previous.log`.

## Layout

| Path | Contents |
|---|---|
| `src/proxy` | `dxgi.dll` proxy entry point and exports, mod initialization |
| `src/core` | Logger, crash forensics, build detection, `addresses.h`, `settings` (the settings table + ini), developer flags |
| `src/hooks` | Shadow vtables (CEG-safe virtual hooks), D3D11 hooks (counts, HUD panel, debug hiding), XInput virtual pad, trace/snapshot logging, old experiments |
| `src/vr` | SteamVR session (connection, poses, submit, flat screen), stereo rendering, dashboard tab, SteamVR Input |
| `docs/re` | Reverse-engineering notes: `FINDINGS.md`, `FUNCTIONS.md` (generated), `TODO_AND_DECISIONS.md`, `avp_symbols.csv` |
| `tools/ghidra` | Headless Ghidra scripts (`run.ps1` wrapper; `ApplySymbols.java` pushes the CSV names into the project) |
| `tools/gen_functions_md.ps1` | Regenerates `docs/re/FUNCTIONS.md` from `avp_symbols.csv` |
| `tools/deploy.ps1` | Copies the built `dxgi.dll` (and `openvr_api.dll` if missing) into the game folder |
| `reference/mohw_port` | Uncompiled copies of the MOHW mod's VR modules; ported pieces are marked in its README |
| `third_party` | MinHook, OpenVR headers |

**Steam CEG rule:** the game's code and read-only data are integrity-checked at level loads, so the mod never patches
the exe image (no inline hooks, no vtable or import-table slot edits). Virtual functions are hooked with per-object
shadow vtables on the heap, everything else inside system DLLs (d3d11, dxgi, xinput1_3) or through data. See
`docs/re/FINDINGS.md` section 10.

## Build

Visual Studio 2022 (v143), Win32:

    MSBuild AvPVR.sln /p:Configuration=Release /p:Platform=Win32

Output: `bin\Release\dxgi.dll`.