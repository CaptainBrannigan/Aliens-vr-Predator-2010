# Aliens vs Predator (2010) VR

A VR mod for **Aliens vs Predator (2010)**, the DX11 executable (`AvP_DX11.exe`, Steam build), through in-process
32-bit SteamVR. Loaded as a `dxgi.dll` proxy from the game folder. Progress and plans: `docs/ROADMAP.md`.

## Status: v0.199 (2026-10-18)

**v0.199: 6DoF, profiles, Predator arms, cutscenes.**
- **Positional head tracking (6DoF)**: the view moves with your head inside an anchored play space. It doesn't move the
  character.
- The **Predator's arms** follow the controllers too (its own bone rig).
- **Profiles**: every setting is kept per class, switched with the class you play. The gun, shot and two-handed values
  are also kept per weapon (pistol and pulse rifle identified by their mesh). The dashboard hides settings the class
  doesn't use, and the hand placement is one set, mirrored for the left hand.
- **Cutscenes and movies** show in full on the flat screen. They were black except for the Predator's cloak shimmer:
  the game's frame has zero alpha there, and the screen is now made opaque. The multiplayer menu goes to the flat
  screen too.
- The **Marine reticle** hide is back (now the GUI pass's first draw). HUD distance is in cm (minimum 1 cm).
- Untested: two-handed aim with the left hand kept relative to the right, and the **Head look** group (the look
  offset with the head aim toggle).

**v0.198: the Marine aims with the gun.** The weapon model is on the right controller and **shots leave the drawn
gun along its barrel** while the view and the look stay on the head (verified in single player and multiplayer).
Every ranged weapon class is covered (the weapons' fire transform, hooked through shadow vtables). Settings in the
**Aiming** group: the shot origin and angle on the gun (cm / degrees, tuned for the pistol), a **laser** along the
shot line for lining them up (off by default; a SteamVR overlay, no hit test, so it shows through walls), and
**two-handed aim**: hold the left grip with the left hand on the barrel and the gun points at it (ported from the
MOHW mod; untested here). Props passing close by no longer get mistaken for the gun.

**v0.15: Marine arms on the motion controllers** (render only). Each hand sits on its controller and two-bone IK
bends the elbows; the shoulders stay on the body. The body, torso and elbows are adjustable in the dashboard's
**Hands** group, the hands' placement in **Hand placement** (one set, mirrored for the left; cm and degrees), the
gun's in **Weapon**. Every setting is kept per class (Marine / Predator / Alien, switched with the class played), the
gun and shot values also per weapon; the dashboard hides what the class playing doesn't use.

**v0.1: head look and stereo are complete** for all three species, single player and multiplayer (multiplayer not yet
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
  camera plus the head's difference, upright on Alien walls and ceilings; a per-class look direction offset.

Not done yet: Alien hands, Alien / Predator attacks, holsters (the weapon in whichever hand grips it), the game's arm
animation switched off (hands locked to the controllers), melee gestures, room-scale walking that moves the character.

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
| Left grip (left hand on the gun's barrel) | Two-handed aim, while held |
| Right grip | Free (melee gestures later); Back unbound |

If you've ever saved your own bindings for the game, SteamVR keeps using that copy: pick "AvP VR defaults" there to
get new actions.

Logs (`avpvr_*.log`) are written next to the game; each launch keeps the previous one as `*.previous.log`.

## Layout

| Path | Contents |
|---|---|
| `src/proxy` | `dxgi.dll` proxy entry point and exports, mod initialization |
| `src/core` | Logger, crash forensics, build detection, `addresses.h`, `settings` (the settings table + ini), developer flags |
| `src/hooks` | Shadow vtables (CEG-safe virtual hooks), D3D11 hooks (counts, HUD panel, debug hiding), first-person body / weapon (bone buffer rewrite: arms IK, torso turn, bone hide, weapon placement), XInput virtual pad, trace/snapshot logging, old experiments |
| `src/vr` | SteamVR session (connection, poses, submit, flat screen), stereo rendering, dashboard tab, SteamVR Input, head aim, shots from the gun, laser overlay |
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