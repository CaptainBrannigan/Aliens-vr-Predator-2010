# AvP VR — roadmap

Target: **Aliens vs Predator (2010), AvP_DX11.exe**, 32-bit, Rebellion's Asura engine, D3D11. Loaded through a
`dxgi.dll` proxy; VR through in-process 32-bit SteamVR (as in the MOHW mod).

## Primary goals

1. **All settings in the SteamVR overlay**, with meaningful names. No ini-only settings.
2. **Stereo rendering for every playable class**: Marine, Xenomorph, Predator (incl. Alien wall-walking and
   Predator vision modes / zoom).
3. **6DoF head tracking.**
4. **Motion controls**: weapons through a virtual gamepad; gestures for melee-oriented actions.
5. **Character mesh IK** (body/arms following head and hands).

## Done

**Framework (phase 0, 2026-10-16):** static mapping (`docs/re/`), dxgi proxy, MinHook, logger, crash forensics,
build verification, log-only trace hooks, game-state snapshots; the Steam CEG constraints found and worked around
(per-object shadow vtables, hooks only inside system DLLs).

**Goal 1 — settings (started):** SteamVR dashboard tab over a table-driven settings store (`src/core/settings.*`,
`src/vr/dashboard.*`), ini rewritten on every change. Groups so far: View, HUD, Controls, Aim offsets, Debug.

**Goal 2 — stereo: COMPLETE (v0.1, all three species; multiplayer untested with a second player):**
- Strategy: native multi-view ruled out (opaque geometry is submitted once for all views); **render-twice** instead:
  the game's frame is the left eye, a second `Frame_BeginFrame` + `Frame_RenderScene` from the Present hook is the
  right eye. Requirements: skip the second job-batch join, leave the D3D lock once more, gameplay frames only.
- Per-eye asymmetric frustum (symmetric render + Submit bounds), aspect through the global projection aspect.
- Eye cameras = the game's camera plus the head's difference from the look it was built from (keeps shake and
  recoil, roll from the head only); the game's own camera goes back into view 0 between frames, for game logic
  that reads it (the Predator's "look at" objectives).
- HUD in a panel inside both eyes (GUI pass detected from the stack, viewport squeezed); marine tracker duplicate
  hidden; pause menu, cutscenes, movies and loading on a flat virtual screen.

**Goal 4 — motion controls (pad and head aim done):** SteamVR Input actions -> virtual XInput pad 0 (hook inside
xinput1_3.dll), generated manifest + Touch bindings; Quest 3 thumbrests = bumpers, Start = held right thumbrest.
Head aim for all species through the look controller's SetLookDirection (`src/vr/head_aim.*`): a player frame
(body vertical + own heading, right-stick turning), per-species aim offsets, local player only.

**Head look: COMPLETE (v0.1)**, including multiplayer (local player only; replication of the head-driven look not yet
verified with a second player). Known issue: the multiplayer pause menu isn't detected as a pause (logged).

**Marine arms on the controllers (v0.15, render only):** the first-person body's bone buffer is rewritten before its
draw (`src/hooks/first_person.*`): hands on the controllers, two-bone arm IK (joints from a vertex read-back), body
offset, torso turn, elbow swivel, bone hide; the weapon model on the right controller (goal 5's arm IK, Marine only).

**Marine controller aim (v0.198):** the ranged weapons' GetFireTransform (`+0xE4`, 14 classes) replaced in shadow
vtables (`src/vr/shots.*`): shots leave the drawn gun along its barrel, the look stays on the head (single player
and multiplayer verified); shot origin / angle offsets, a laser overlay along the shot line, two-handed aim (left
grip on the barrel, from the MOHW mod).

## Next

1. **6DoF head position** (goal 3), port `reference/mohw_port/hooks/head_position.*`.
2. **Alien and Predator**: hands / attacks on the controllers (their first-person bodies and attack code).
3. Holsters (the weapon in whichever hand grips it), per-weapon shot origins, the game's arm animation off, melee
   gestures.
4. Multiplayer pause menu on the flat screen (from the logged menu state).
5. Predator vision / zoom (goal 2).

## DX9 (AvP.exe)

Possible later, not free. The game code is the same but it is a **different binary** (stamp 0x4C34AB3D, image size
0x953000): every address must be re-found (signatures/Ghidra on that exe), the present/capture path is D3D9 (SteamVR
needs a D3D11 texture → shared-surface interop), and the dxgi proxy never loads there (it doesn't use DXGI) — it
would need a `dinput8.dll` or `xinput1_3.dll` proxy, which both exes import. Plan: finish DX11, then decide.