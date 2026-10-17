# AvP VR — decisions + live to-do (2026-10-16, status updated 2026-10-17)

Context: `FINDINGS.md` (what was found), `FUNCTIONS.md` (all 84 named functions + 37 globals, also applied in Ghidra).
Addresses are x32dbg addresses; computed absolute addresses are in parentheses.

---

## A. Decisions — answered 2026-10-16

A1 separate repo (`C:\Dev\VR Mod\AVP2010\main`), copy MOHW's generic pieces (in `reference/mohw_port/`) ·
A2 dxgi proxy · A3 engine two-view first · A4 in-process SteamVR · A5 DX11 first, DX9 later (see `docs/ROADMAP.md`) ·
A6 goals in `docs/ROADMAP.md` · A7 log-only hooks en masse (built: `src/hooks/trace_hooks.cpp`) · A8 repo above ·
A9 memory saved. Original question text kept below for context.

**Later decisions (2026-10-16/17):** skip `noceg`, stay fully CEG-safe · render-twice stereo (native multi-view
ruled out, section B3) · HUD stays in the eyes, squeezed into a panel (no separate HUD overlay window); the SteamVR
overlay is for the mod's settings · Touch thumbrest touch = LB / RB (instead of MOHW's grip double-tap) · next:
head look drives the player's aim.

**A1. Project home and code reuse.** *My pick: new solution in `C:\Dev\VR Mod\AVP2010\mod\`, copying MOHW's generic modules.*
Reusable as-is or nearly: `openvr_direct` (32-bit in-process SteamVR), `vr_input`, `vr_overlay` (dashboard tab),
`settings`, `logging`, `xinput_hook`, `present_hook`, MinHook, the `proxy_dll` scaffold. MOHW-specific hooks stay behind.
Alternative: one repo with both games (shared lib + per-game folders) — cleaner long-term, more refactoring now.

**A2. Loader DLL.** *My pick: `dxgi.dll` proxy, same as MOHW.*
AvP_DX11.exe loads DXGI through d3d11.dll, so an app-folder dxgi.dll should load the same way it does for MOHW (verify on first run).
Alternatives: `xinput1_3.dll` or `dinput8.dll` proxy (both imported directly by the exe, tiny export lists).

**A3. Stereo strategy.** *My pick: test the engine's own multi-view first (B3), fall back to MOHW's alternating-eye.*
- Native multi-view: if `g_ViewCount = 2` renders two viewports, both eyes come out of one frame — no 2× latency, no alternating-eye jitter.
- Alternating eye: known to work (MOHW), costs half the per-eye rate.
- Calling the scene render twice per frame: possible later, more invasive.

**A4. VR runtime path.** *My pick: in-process SteamVR direct (MOHW's `openvr_direct`), no companion process.*
Same 32-bit SteamVR runtime that works for MOHW.

**A5. Target exe.** *My pick: AvP_DX11.exe only*, ignore the DX9 `AvP.exe`. Confirm you always launch the DX11 one.

**A6. First milestone scope.** *My pick: head-tracked stereo in gameplay + HUD readable; controllers after.*
Order after that: controller aim via `LookCtrl_SetLookDirection`'s aim point → virtual pad → hands/weapon → per-species (Alien wall-walk, Predator vision/zoom).

**A7. Mod skeleton while you're away next time?** I can write the proxy + MinHook + logging + *log-only* hooks on
`Cam_SetRotationMatrix`, `Cam_SetHorizontalFov`, `LookCtrl_SetLookDirection`, `D3D_PresentFrame` so the first live session
reads logs instead of single-stepping. Needs A1/A2 answered first.

**A8. Git.** Init a repo for the AvP project (analysis + scripts + future mod)? Nothing has been committed anywhere.

**A9. Memory.** OK to save an AvP project memory (paths, "not packed", camera/look/present addresses, Ghidra script workflow)?

---

## B. Live to-do (needs you + the game)

**B1. Console.** *(open)* Try `~`, `` ` ``, `F1`–`F12`, `Insert`, `Home` in a level. If none open it: in x32dbg, watch `009E1C58` and
`009E15C8` while pressing keys — `Console_IsAllowed` needs the first nonzero and the second zero.
If it opens, try `AVP.FlyCam 1` and `Visibility.RenderEverything 1`. Worth doing first: it makes later tests much faster.

**B2. Confirm the camera writers.** *(done: `CamComp_ApplyToRenderCamera` through the shadow vtable is the per-frame
gameplay writer; cutscenes use their own path. Alien wall-walk writer still unchecked, see B6.)* Breakpoint `Cam_SetRotationMatrix` (`004FA170`) and `Cam_SetPosition` (`004FA050`) in gameplay;
expect `CamComp_ApplyToRenderCamera` (`00695760`) every frame. Note any other caller (cutscenes, Alien wall-walk, ladders, finishers).

**B3. Multi-view test (the big one).** *(done 2026-10-16: ruled out -- opaque geometry renders once for all views;
render-twice used instead, FINDINGS section 11.)* Per-view arrays are allocated at startup from `g_ViewCount`, so set it before the game initializes:
- Easiest: patch a **copy** of AvP_DX11.exe — file offset `0x5CCD4C` (VA `009CCD4C`), byte `01` → `02`. Keep the original.
- Then in a level check, in x32dbg: view 1's active byte at `009DB2F4 + 0x114` (`009DB408`), view 1's viewport rect at
  `009DB2F4 + 0x3C` (`009DB330`), and view 0's at `009DB1D4`.
- Outcomes: crash (code assumes 1 somewhere — the array itself has 4 slots, so it's not an overflow), nothing visible
  (view 1 inactive — try setting `009DB408` to 1), or two images (jackpot).
- Side-by-side rects (4 floats each: scaleX, scaleY, offsetX, offsetY): view 0 `009DB1D4` = `0.5, 1, 0, 0`;
  view 1 `009DB330` = `0.5, 1, 0.5, 0`. View 1 also needs a camera: copy view 0's pos/rotation (`009DB198`, 0x30 bytes)
  to `009DB2F4` to see the same image twice, then offset it for an eye.

**B4. Viewport → GPU path.** *(superseded: stereo renders full frames; the HUD panel squeezes D3D viewports directly.)* Hardware breakpoint on read of `009DB1D4` (view 0 viewport rect) to find the function that turns the
per-view rect into a D3D viewport. Needed for B3's side-by-side and for any stereo approach.

**B5. Look path.** *(next: head aim. Planned as log-only shadow hooks on the look controller instead of breakpoints.)* Breakpoint `LookCtrl_SetLookDirection` (`006AC1E0`); `ecx` = controller, quaternion at `[ecx+0x70]`.
Confirm it fires for the player each tick and which of the 4 subclass vtables the player uses (Marine/Alien/Predator).

**B6. Alien wall-walk camera.** *(open, now a known bug: the eye camera keeps only the game's yaw, which spins on walls
and at steep pitch; fix planned with head aim, ROADMAP "Next" 2.)* While wall-walking, watch the derived roll at `009DB198 + 0x38` (`009DB1D0`) and who writes the rotation.
This decides how VR handles arbitrary "up" vectors (comfort).

**B7. Present.** *(done: the HUD is drawn into the scene target by the GUI pass inside `Scene_RenderFrame`,
FINDINGS section 12.)* Breakpoint `D3D_PresentFrame` (`0066A840`) once; confirm `g_SceneTarget` (`[00A25828]`) is the full-res frame
and whether the HUD is already in it (decides where per-eye HUD placement happens).

**B8. First-person hands/weapon.** *(open)* Break in `Char_SelectSkinAttachment` (`006A6450`) when a level loads; check `[ecx+0xE8]` bit 2
and what `[ecx+0x98]` points to — starting point for the viewmodel.

---

## C. What I can still do statically without you

- Map the remaining first-person weapon path (weapon component → render).
- ~~Find the end of the view-camera array~~ — done: 4 slots (see FINDINGS §6).
- ~~Locate the HUD/GUI render pass~~ — done: `Gui_RenderPass` (`0079C480`), FINDINGS section 12.
- Tie the scene passes the user identified (lighting, props, floors + walls) to their addresses.
- Decompile the 4 look-controller subclasses to tie each to a species.
