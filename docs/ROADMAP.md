# AvP VR — roadmap

Target: **Aliens vs Predator (2010), AvP_DX11.exe**, 32-bit, Rebellion's Asura engine, D3D11. Loaded through a
`dxgi.dll` proxy; VR through in-process 32-bit SteamVR (as in the MOHW mod).

## Primary goals

Each gets its own sub-goal list once the framework is in place.

1. **All settings in the SteamVR overlay**, with meaningful names. No ini-only settings.
2. **Stereo rendering for every playable class**: Marine, Xenomorph, Predator (incl. Alien wall-walking and
   Predator vision modes / zoom).
3. **6DoF head tracking.**
4. **Motion controls**: weapons through a virtual gamepad; gestures for melee-oriented actions.
5. **Character mesh IK** (body/arms following head and hands).

## Phase 0 — framework (current)

- [x] Static mapping of camera, views, look, input, present, console (`docs/re/`)
- [x] Repo, solution, dxgi proxy, MinHook, logger, build verification
- [x] Log-only trace hooks on ~55 identified functions + periodic game-state snapshot
- [ ] First live run: read `avpvr_trace.log` / `avpvr_snapshot.log`, confirm callers and per-frame order
- [ ] Two-view experiment (`dev::kTwoViewExperiment`) — decides the stereo strategy
- [ ] Port from MOHW: `openvr_direct` (SteamVR connection, submit), `vr_overlay` + settings table, `vr_input`
      (controller profiles), virtual XInput pad, master switch (`reference/mohw_port/`)

## Stereo strategy (decided 2026-10-16)

Engine's own multi-view first: the Asura renderer has 4 view slots with per-view cameras, viewport rects,
visibility masks and render-queue packets (split-screen heritage). If two views render, both eyes come from one
frame. Fallback: MOHW-style alternating eye.

## DX9 (AvP.exe)

Possible later, not free. The game code is the same but it is a **different binary** (stamp 0x4C34AB3D, image size
0x953000): every address must be re-found (signatures/Ghidra on that exe), the present/capture path is D3D9 (SteamVR
needs a D3D11 texture → shared-surface interop), and the dxgi proxy never loads there (it doesn't use DXGI) — it
would need a `dinput8.dll` or `xinput1_3.dll` proxy, which both exes import. Plan: finish DX11, then decide.
