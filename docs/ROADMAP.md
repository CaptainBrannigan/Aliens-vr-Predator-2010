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
`src/vr/dashboard.*`), ini rewritten on every change. Groups so far: View, HUD, Controls, Debug.

**Goal 2 — stereo (Marine, Predator working):**
- Strategy: native multi-view ruled out (opaque geometry is submitted once for all views); **render-twice** instead:
  the game's frame is the left eye, a second `Frame_BeginFrame` + `Frame_RenderScene` from the Present hook is the
  right eye. Requirements: skip the second job-batch join, leave the D3D lock once more, gameplay frames only.
- Per-eye asymmetric frustum (symmetric render + Submit bounds), aspect through the global projection aspect.
- Head orientation on the camera (game yaw + head yaw, head pitch/roll), persistent wider camera between frames
  for the engine's pre-camera visibility work.
- HUD in a panel inside both eyes (GUI pass detected from the stack, viewport squeezed); marine tracker duplicate
  hidden; pause menu, cutscenes, movies and loading on a flat virtual screen.

**Goal 4 — motion controls (pad done):** SteamVR Input actions -> virtual XInput pad 0 (hook inside xinput1_3.dll),
generated manifest + Touch bindings; Quest 3 thumbrests = bumpers.

## Next

1. **Head look drives the player's aim** — the look controller (`LookCtrl_*`, `docs/re/FINDINGS.md` section 2) is a
   heap object with a vtable: shadow-hook `+0x60` (look update) / `+0x9C` (SetLookDirection). Static pass, then a
   log-only shadow-hook build to pin the per-species subclass, the direction format and the accumulated yaw/pitch.
   MOHW design: additive head yaw, absolute head pitch, stick pitch zeroed.
2. **Alien camera:** with the aim following the head, render the game's full camera orientation (incl. wall-walk
   roll) plus a small head-latency correction, instead of game yaw + head angles (spins on walls / steep pitch now).
3. **6DoF head position** (goal 3), port `reference/mohw_port/hooks/head_position.*`.
4. **Motion-controller aim** (goal 4): the look controller's aim-point raycast (character vtable `+0xBC`).
5. Melee gestures, Predator vision / zoom, IK (goals 4, 2, 5).

## DX9 (AvP.exe)

Possible later, not free. The game code is the same but it is a **different binary** (stamp 0x4C34AB3D, image size
0x953000): every address must be re-found (signatures/Ghidra on that exe), the present/capture path is D3D9 (SteamVR
needs a D3D11 texture → shared-surface interop), and the dxgi proxy never loads there (it doesn't use DXGI) — it
would need a `dinput8.dll` or `xinput1_3.dll` proxy, which both exes import. Plan: finish DX11, then decide.