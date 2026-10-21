# AvP VR — development notes

How the mod is built and laid out, and its version history. The player-facing description is in
[README.md](README.md); progress and plans in [docs/ROADMAP.md](docs/ROADMAP.md); reverse-engineering notes in
[docs/re](docs/re).

## Build

Visual Studio 2022 (v143), Win32:

    MSBuild AvPVR.sln /p:Configuration=Release /p:Platform=Win32

Output: `bin\Release\dxgi.dll`. `tools\deploy.ps1` copies it (and a 32-bit `openvr_api.dll` if the game folder has
none) into the game folder; `-Remove` takes the mod out.

Logs (`avpvr_*.log`) are written next to the game; each launch keeps the previous one as `*.previous.log`.

## Steam CEG rule

The game's code and read-only data are integrity-checked at level loads, so the mod never patches the exe image (no
inline hooks, no vtable or import-table slot edits). Virtual functions are hooked with per-object shadow vtables on
the heap, everything else inside system DLLs (d3d11, dxgi, xinput1_3, binkw32) or through data. See
`docs/re/FINDINGS.md` section 10.

## Layout

| Path | Contents |
|---|---|
| `src/proxy` | `dxgi.dll` proxy entry point and exports, mod initialization |
| `src/core` | Logger, crash forensics, build detection, `addresses.h`, `settings` (the settings table, profiles + ini), developer flags |
| `src/hooks` | Shadow vtables (CEG-safe virtual hooks), D3D11 hooks (counts, HUD panel, debug hiding), first-person body / weapon (bone buffer rewrite: arm IK, animation layering, torso turn, bone hide, weapon placement), XInput virtual pad, Bink movies, screen warp effects, trace/snapshot logging, old experiments |
| `src/vr` | SteamVR session (connection, poses, submit, flat screen and its opaque pass), stereo rendering and head position, dashboard tab, SteamVR Input, head aim, shots from the gun, laser overlay |
| `docs/re` | Reverse-engineering notes: `FINDINGS.md`, `FUNCTIONS.md` (generated), `TODO_AND_DECISIONS.md`, `avp_symbols.csv` |
| `tools/ghidra` | Headless Ghidra scripts (`run.ps1` wrapper; `ApplySymbols.java` pushes the CSV names into the project) |
| `tools/gen_functions_md.ps1` | Regenerates `docs/re/FUNCTIONS.md` from `avp_symbols.csv` |
| `tools/deploy.ps1` | Copies the built `dxgi.dll` (and `openvr_api.dll` if missing) into the game folder |
| `reference/mohw_port` | Uncompiled copies of the MOHW mod's VR modules; ported pieces are marked in its README |
| `third_party` | MinHook, OpenVR headers |

## Version history

**Beta (2026-10-18)**
- Grips are the bumpers; double-tap the left thumbrest for Back, the right for Start; both thumbrests together recenter. The
  left grip doesn't press LB while the left hand is at the Marine's gun (two-handed grab).
- The hands take the game's animation on top of the controllers (attacks, reloads, recoil; setting "Hands also move
  with the game's animation"); the Marine's gun moves with the right hand.
- Room-scale stepping: the character steps under the head past a set distance, stops on arrival, gives up when blocked;
  the coast after the player's own stick is carried (the old walk circled the spot).
- Screen warp effects (the Alien's fisheye) skipped in VR by default (setting "Screen warp effects"). That didn't fix
  the Alien's doubling: distant things (the view through windows) shift slightly against head movement, nearby
  geometry doesn't -- open.
- Built-in defaults = the author's tuned values, per class.

**v0.199: 6DoF, profiles, Predator arms, cutscenes.**
- Positional head tracking (6DoF): the view moves with the head inside an anchored play space.
- The Predator's arms follow the controllers too (its own bone rig).
- Profiles: every setting kept per class, switched with the class played; the gun, shot and two-handed values also
  per weapon (pistol and pulse rifle identified by their mesh). The dashboard hides settings the class doesn't use;
  the hand placement is one set, mirrored for the left hand.
- Cutscenes and movies show in full on the flat screen (the game's frame has zero alpha there; the screen is made
  opaque). The multiplayer menu goes to the flat screen too.
- The Marine reticle hide is back; HUD distance in cm.

**v0.198: the Marine aims with the gun.** The weapon model on the right controller; shots leave the drawn gun along
its barrel while the view and the look stay on the head (single player and multiplayer). Every ranged weapon class
(the weapons' fire transform, through shadow vtables). Shot origin / angle offsets, a laser along the shot line, and
two-handed aim (ported from the MOHW mod).

**v0.15: Marine arms on the motion controllers** (render only): hands on the controllers, two-bone IK for the elbows,
body offset, torso turn, elbow swivel.

**v0.1: head look and stereo** for all three species, single player and multiplayer: render-twice stereo with
per-eye asymmetric frusta, the HUD in a panel, menus / cutscenes / movies / loading on a flat screen, the SteamVR
dashboard tab, Touch controllers as a virtual Xbox pad, head aim (upright on Alien walls and ceilings).
