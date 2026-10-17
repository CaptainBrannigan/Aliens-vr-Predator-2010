# AvP 2010 (AvP_DX11.exe) — first static pass, 2026-10-16

Program: `AvP_DX11.exe` (original, unpacked; identical code to the Scylla dump), image base `00400000`.
All addresses map 1:1 to x32dbg. Ghidra project: `C:\Dev\VR Mod\AVP2010\GhidraProjectFiles\AVP2010.gpr`.
No RTTI in the binary. Names below are mine, inferred from decompiles — confidence noted per item.

Raw outputs in this folder: `keyword_xrefs.txt`, `batch1..6.txt` (decompiles), `xrefs1..4.txt`.
Scripts in `..\GhidraScripts\`: `KeywordStringXrefs.java`, `DecompileList.java` (decompile + callers/callees),
`XrefsAndVtables.java` (`x:ADDR` xrefs, `v:ADDR:N` vtable dump).

---

## 1. Render camera — the main injection point (high confidence)

`PTR_DAT_009ccd48` → **current view's camera struct**. Read in 116 functions.
It points into an **array of view cameras** at `009DB198`, stride `0x15C`:

| Global | Meaning |
|---|---|
| `009CCD48` | pointer to current view camera (initial value `009DB198`) |
| `009CCD4C` | view count — **1** in the exe, never written by code |
| `009CCD50` | current view index |
| `009DB190` | current view bit mask (`1 << index`) |
| `009CCD58` | camera dirty flag (set by every setter) |

Camera struct layout (view 0 absolute address in parentheses):

| Offset | Field |
|---|---|
| `+0x00` (`009DB198`) | position vec3 |
| `+0x0C` (`009DB1A4`) | 3×3 rotation, 9 floats |
| `+0x30/+0x34/+0x38` (`009DB1C8/009DB1CC/009DB1D0`) | derived pitch / yaw / roll (asin/atan2 of the matrix) |
| `+0x50..+0x88` (`009DB1E8..009DB220`) | 5 frustum plane normals (built by recompute) |
| `+0x8C..+0xC4` | abs() of those normals (culling) |
| `+0xC8..+0xD8` (`009DB260..009DB270`) | frustum plane distances |
| `+0xDC` / `+0xE0` (`009DB274/009DB278`) | probably far / near clip (set via `FUN_004f9320` / `FUN_004f9340`) |
| `+0xE4` (`009DB27C`) | aspect (runtime dump: 1.7778) ; `+0xE8` = 1/aspect |
| `+0xEC` (`009DB284`) | **horizontal half-FOV, radians** (dump: 0.871) ; `+0xF0` 1/cos, `+0xF4` tan, `+0xF8` 1/tan |
| `+0xFC` (`009DB294`) | **vertical half-FOV, radians** (dump: 0.589 → 67.5° vertical) ; `+0x100` 1/cos, `+0x104` tan, `+0x108` 1/tan |

Setters (all write through `009CCD48`, set the dirty flag, most call recompute):

| Function | Role |
|---|---|
| `FUN_004fa050(float* pos3)` | set position (+ plane distances) |
| `FUN_004fa170(float* mat9)` | set rotation matrix, derive Euler, recompute |
| `FUN_004fa2a0(float* quat)` | set rotation from quaternion (FlyCam path) |
| `FUN_004fa3e0(hHalfFov, aspectScale)` | set horizontal FOV, derive vertical |
| `FUN_004fa560(vHalfFov, aspectScale)` | set vertical FOV, derive horizontal |
| `FUN_004f9590` | recompute frustum planes from rotation + FOVs |
| `FUN_004fa6e0` | camera defaults init |

Who drives the camera:

| Function | Role | Confidence |
|---|---|---|
| `FUN_00695760` | **gameplay camera apply** (camera component vtable `+0x08`): builds pos/rot/FOV and calls the setters above. Has FlyCam/FixCam branches | high |
| `FUN_00695d10` | camera component update (vtable `+0x0C`): spring/collision position smoothing. Gated by cvar `AVP.Camera.Update` (`009D2828`) | high |
| `FUN_006ff350` | entity tick that calls the two above every frame via component at `entity+0x28` (shared by 16 entity vtables) | high |
| `FUN_0052a160` | cinematic camera: pos `+0x38`, matrix `+0x44`, vFOV `+0x9C`; only when `DAT_009ea24d` | high |
| `FUN_0063e6b0` | camera shake, applied per view by modifying the rotation | high |
| `FUN_006f54d0` | copies pos+matrix from an object into the camera (caller `FUN_006ef6e0`) — unidentified | open |
| `FUN_00617f15` | saves camera → renders something with a temp camera → restores (render-to-texture? Predator mask / scope?) | medium |
| `FUN_005d8790` | viewport/letterbox/aspect setup from screen size (`DAT_009faafc` w, `DAT_009fab00` h); `FUN_004f9360` = viewport rect | medium |

Camera component class: vtable `0091D694` (ctor `FUN_00696d00`). Four more subclasses share
the apply/update slots: `00920054`, `00921360`, `009226E4`, `00923280` (probably per-species/vehicle).

`DAT_009ea24d` = **"cinematic playing" flag** (gates the cutscene camera and the player look/input code) — medium-high.

### Multi-view lead (unverified, potentially big)
The engine loops over views (`FUN_0063e6b0`, `FUN_00662780`, `FUN_0075cc60` allocates per-view arrays).
The runtime dump shows a second, identity-initialized camera struct at `009DB2F4` (= `009DB198 + 0x15C`).
Split-screen heritage, probably. **Live test:** set `009CCD4C` to 2 early (or patch the data section) and see whether
the game renders two viewports. If it does, that's a native two-eye render path, cheaper than alternating eyes.

---

## 2. Player look / aim (high confidence on roles, offsets need live check)

Look-controller base class: vtable `0091FAB8` (ctor `FUN_0069e570`, registers `Controls.*` sensitivity/aim-assist cvars).
Concrete subclass: vtable `00920D70`; three more share the main look methods (`009220F8`, `00922E40`, `00923C88`)
— likely Marine / Alien / Predator.

| Function | Role |
|---|---|
| `FUN_006acba0` | subclass per-tick update (vtable `+0x08`), calls base `FUN_0069f650` |
| `FUN_0069f650` | base per-tick: computes turn speeds/aim assist, then calls `+0x60` |
| **`FUN_006ace60`** | **look update** (vtable `+0x60`): reads look actions 0–3 (`yaw = act1 + act3`, `pitch = act0 + act2`), × profile sensitivity (`DAT_00a71e14+0xCC/+0xD0`), aim assist, pitch clamp (`ctrl+0xD0/+0xD4`), yaw wrap, builds look direction, calls `+0x9C` |
| **`FUN_006ac1e0`** | **SetLookDirection** (vtable `+0x9C`): stores the look quaternion at `ctrl+0x70..+0x7C`, pushes it to the character (`character vtable +0xB4`), raycasts an aim point from the eye (`character vtable +0xBC`) |

→ Equivalent of MOHW's AimingController. Head look = override the direction passed to `FUN_006ac1e0`
(or the yaw/pitch inside `FUN_006ace60`), with the same additive-yaw / absolute-pitch design as MOHW.
The `+0xBC` aim-point call is the likely spot for **motion-controller aim**.

`DAT_00a71e14` = player profile/settings object: `+0xB5` invert Y, `+0xBA/+0xBB` aim/look assist on, `+0xCC/+0xD0` sensitivities.

---

## 3. Input (high confidence)

| Function | Role |
|---|---|
| `FUN_0060ded0(this, idx)` | read action value: `return idx < this[1] ? this[4][idx] : 0` — a float array of action states |
| `FUN_0060df40(idx)` | consume/clear an action |
| `FUN_0069ebb0(actionIdx, ?, consume)` | look controller's action read (checks UI/cinematic blocks, then `FUN_0060ded0`). 130 callers |
| `FUN_006256a0` | 35 action reads — probably the action-map builder; worth decompiling first when doing controllers |
| `FUN_00644f70` | pad-name → type (DirectInput names, XInput = 5) |

Look action indices: 0/2 = pitch pair, 1/3 = yaw pair. FlyCam uses its own map (0–2 move, 3–5 rotate, 6–8 speed, 9 roll reset, 10 invert).
This action array is the natural place to inject VR controller input (like MOHW's virtual pad, but one layer up).

---

## 4. Useful console variables (cvar registration: `FUN_0058c960` bool, `FUN_0058c9e0` float; `FUN_0058b7e0` exists-check)

| Cvar | Address | Use |
|---|---|---|
| `AVP.FlyCam` | `00A66534` (bool) | free camera — handy for stereo/IPD testing |
| `AVP.FixCam` | `00A66535` (bool) | freeze camera |
| `AVP.FlyCam.UseFOV` / `AVP.FlyCam.FOV` | `00A66536` / `009CFD74` | FlyCam FOV override |
| `AVP.Camera.Update` | `009D2828` (bool) | gates the camera position update |
| `Visibility.FreezeCameraNode` | `009FF318` (bool) | freezes the visibility/portal camera — relevant if the 2nd eye gets culled wrongly |
| `Visibility.RenderEverything` | `009FF31B` (bool) | disables visibility culling — quick test for stereo culling bugs |

Console commands exist too (`Camera.Shake.Start/Stop`, registered via `FUN_0058c900`).

---

## Suggested live-debugging order

1. Breakpoint `FUN_004fa170` / `FUN_004fa050` in gameplay: confirm `FUN_00695760` is the per-frame caller, and see what else writes the camera (cutscenes, Alien wall-walk).
2. Watch `009DB198` (pos) / `009DB1A4` (rotation) / `009DB284` (hFOV) in gameplay and confirm the values move as expected.
3. Try the multi-view test (`009CCD4C = 2`).
4. Breakpoint `FUN_006ac1e0`, read the quaternion at `ctrl+0x70` (`[ecx+0x70]`) while looking around, and confirm it's the player.
5. Toggle `AVP.FlyCam` / `Visibility.RenderEverything` if the console is reachable.

Open: `FUN_006f54d0`'s source object, what `FUN_00617f15` renders.

---

# Second pass (same day)

All identified functions/globals are now **named in the Ghidra project** (`ApplySymbols.java` from `avp_symbols.csv`);
the readable index is `FUNCTIONS.md`. Below, names are the applied ones.

## 5. D3D11 device and present (high confidence)

- `D3D_CreateDeviceAndSwapChain` (`0066C060`) is the only `D3D11CreateDeviceAndSwapChain` call. From the push order:
  **`g_D3DDevice` = `00A2581C`, `g_D3DContext` = `00A25820`, `g_SwapChain` = `00A25824`**.
  Swapchain: `g_BackBufferWidth`×`g_BackBufferHeight` (`00A25814`/`00A25818`), R8G8B8A8_UNORM_SRGB, 2 buffers, 60 Hz.
  No DXGI factory import — everything comes from that one call.
- `D3D_PresentFrame` (`0066A840`): `context->CopyResource(g_BackBuffer 00A2582C, g_SceneTarget 00A25828)` then
  `swapchain->Present(g_VSync, 0)`. **The game renders into its own offscreen target and copies it to the backbuffer** —
  that copy is the natural per-eye capture point (hook `D3D_PresentFrame`, or the swapchain's Present).
- Chain: `Render_EndFrame` (`005D8C00`) → `Render_FinishAndPresent` (`0067BF00`) → `D3D_PresentFrame`.

## 6. Multi-view rendering — deeper (static: strong; live: untested)

The engine is multi-view aware past culling, into the render queue:
- `Mesh_SubmitForViews` (`004E8F50`) walks every view, checks the view's **active byte at `cam+0x114`** (view 0: `009DB2AC`),
  builds a view bitmask and queues the mesh.
- `RenderQueue_SubmitMeshPerView` (`005D90B0`) / `RenderQueue_SubmitPerView` (`005D8FD0`) emit **one draw packet per view bit**
  with a per-view distance sort key (`View_SelectAndSortKey`).
- Decals (`Decal_SubmitForViews`), shadow/reflection passes (`ShadowOrReflect_RenderPerView`) do the same.
- `g_RenderOnlyCurrentView` (`009D9008`) restricts loops to the current view.
- Per-view viewport: `Cam_SetViewportRect` writes 4 floats at `cam+0x3C` (view 0: `009DB1D4`).
- `g_ViewCount` (`009CCD4C`) = 1 is initialized data and never written.
- **The array has exactly 4 slots** (`009DB198`, `009DB2F4`, `009DB450`, `009DB5AC`): the next unrelated global
  (`g_FlyCamInvertPitch` `009DB708`) sits at `+0x570` = 4 × `0x15C`, and slots 1–2 are referenced in code.
  4-player split-screen capacity — setting the count to 2 stays in bounds.
- **Viewport rect layout confirmed** by `Gui_MapRectToViewViewport` (`005D3F80`): `cam+0x3C` scaleX, `+0x40` scaleY,
  `+0x44` offsetX, `+0x48` offsetY (normalized, default 1,1,0,0). Side-by-side = view 0 `(0.5,1,0,0)`, view 1 `(0.5,1,0.5,0)`.
  That function maps HUD/2D drawing into the current view's viewport, so the HUD is per-view aware as well.
- `Render_BeginFrame`'s half-width mode (`g_HalfWidthMode` `009EA257`) is dead (BSS, never written).

Not found statically: the GPU-side loop that sets each view's D3D viewport from `cam+0x3C` and executes its packets.
That's what the live test answers.

## 7. Input — XInput layer (high confidence)

- `XInput_PollPad` (`00645AA0`) is the **only `XInputGetState` caller**: normalized sticks → `pad+0x248..+0x254`,
  buttons → `pad+0x258`, triggers → `pad+0x260/+0x261`, then fans out to button bytes `pad+0x1EC..`.
  Hooking the `XInputGetState` IAT slot (`008BC444`) is the MOHW-style virtual-pad route.
- `XInput_SetRumble` (`00659FB0`) is the only `XInputSetState` caller (driven by camera shake) — haptics hook.
- DirectInput8 is also imported (`FUN_00644530`), for keyboard/mouse and DirectInput pads.
- `Input_PackActionBits` packs actions 0..32 into a bitmask; action **names are not in the exe** (data-driven,
  probably in the `.asr` archives).

## 8. Dev console exists in the shipping build (high confidence on code, unknown how to open)

- `Console_Init` (`0058CA80`) creates it (`g_Console` `009E77A0`, vtable `008CBA10`), registers Clear/Help/etc.
- `Console_Tick` (`0058CE80`) opens it when `Console_IsAllowed` (`0079D4E0`): `009E1C58 != 0 && 009E15C8 == 0 && 009FFD38 == 0`.
  Neither of the first two is written directly — likely fields of an input/key-state struct.
- `Console_Show(show, instant)` = vtable `+0x14` (`0058D6B0`); a mod could just call it.
- If it opens, every cvar in §4 is live: FlyCam, `Visibility.RenderEverything`, etc.

## 9. First-person body/arms (lead only)

- `Char_SelectSkinAttachment` (`006A6450`): picks `FirstPerson.SkinAttachment` vs `ThirdPerson.SkinAttachment` by
  `character+0xE8` bit 2 → `character+0x98` is the skin object. Referenced from 6 character vtables (slots at `0091FF8C`, `009212CC`, `009240AC`, `0092424C`, `00924424`, `009245CC`).
- `Entity_CreateVisionAndWeaponComps` (`007062C0`): creates the VisionMode and Weapon components.
- The weapon/hands render path and whether the viewmodel uses its own FOV are still unknown — needs live work.
