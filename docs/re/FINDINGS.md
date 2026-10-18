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
Intermediate `00920D70` (ctor `006AB140`), then one class per species, made by `FUN_0081A800` (which recreates the
player's controller whenever the connected-pad count changes, stored at `playerEntity+0x30`): **Predator `00923C88`,
Alien `009220F8`, Marine `00922E40`** (live 2026-10-17; `g_PlayerSpecies` `009D4BA0` = 0 / 1 / 2). Every instance has
`ctrl+0x60` pointing at `ctrl+0x30` or `ctrl+0x48` (the shadow-vtable validator).

**Live (2026-10-17, log-only shadow hook on `+0x9C`, `src/hooks/look_probe.cpp`) — corrections to the table below:**
- `SetLookDirection` runs ~115 times a second (every rendered frame), always from `UpdateLook` (`006ADE3A`).
- Its argument is a **world-space unit direction (x, y, z, 0)**, not a quaternion. **The engine's world y points
  DOWN** (y > 0 = looking down; matrix column 1 -- camera and body -- is the "down" axis, the projection's negative
  P11 flips it back for the screen; OpenVR -> engine is a 180-degree turn about x, hence head yaw -1 / pitch +1 /
  roll -1). Engine angles: + pitch = up, + yaw = right.
- `ctrl+0x20` = **pitch** input (actions 1 + 3, where invert-Y applies), `ctrl+0x24` = **yaw** input (actions 0 + 2).
- No stored yaw/pitch: each tick starts from the character's current look in the **body frame** — a 3x3 from the
  character's virtual base (`character + 4 + vbtable[2]`, vtable `+0x0C`, zero-argument thiscall; local = M^T world,
  M's columns = body axes) — adds the stick deltas, clamps, and transforms back. The same orientation is a quaternion
  at `[character+0x3C] + 0xA8`.
- Pitch limits relative to the body (UpdateLook clamps asin(-y_local) to [-(90° − `+0xD4`), 90° − `+0xD0`]):
  up 90° − `+0xD0`, down 90° − `+0xD4`: Marine 85 up / 75 down, Predator 85 up / 80 down.
  **Yaw relative to the body is capped at about ±90°; past it the body itself turns** (all species).
- Marine / Predator: body down axis = world down (0, 1, 0). **Alien: the body frame follows the surface** (walls:
  rotated; ceiling: down axis (0, −1, 0), i.e. feet towards the ceiling); the look stays constant in body-local terms
  while the body rotates onto a new surface.
- The Alien's tutorial "look around" objective reads the render camera (it passed with head pitch, not stick pitch,
  while the VR camera ignored the game's pitch); the Marine's reads the look.

**Head aim (2026-10-17, `src/vr/head_aim.cpp`):** `+0x9C` replaced in the three species' shadow vtables. The mod keeps
a player frame (up = the body's up, heading turned only by the right stick and carried across surface changes) and
hands the original setter forward(frame × head), clamped to the controller's pitch limits; the eye cameras use the same
matrix. Only the **local player's** controller is steered: `EntityRegistry_Find` (`004119D0`, thiscall on
`00A6DC80`, `(handle, 1)`, `RET 8`) with the handle at `[g_GameScene (00A71C50)] + 8` gives the player entity, its
controller at `+0x30`; other controllers (other players' characters in multiplayer) are left alone.

**Netcode (static, 2026-10-17): open.** Network strings cover only connection management (Asura NetLink status and
connect/disconnect, `Network.TimeoutMilliseconds`, a `NetSim` latency / packet-loss simulator with console commands)
-- nothing on what a client sends for its look. Head aim goes through the game's own setter (stored at `ctrl+0x70`,
pushed to the character via `+0xB4`), so state-based replication (the usual "client sends its view direction")
carries it; if the host re-simulated look from stick input instead, it would see the game's own stick look. Next:
the character's `+0xB0/+0xB4` (vtable logged in `avpvr_aim.log` at activation) and their readers, and a live
two-player check.

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

**Static pass (2026-10-17, start of the motion-controls work):**
- A character has **up to three skinned mesh parts** at `character+0x1C0..+0x1C8`, picked from a model-name list by
  mode: first person (`+0xE8` bit 2) list `00920034`, third person `00920028` (bit 3) or `0092001C`; each name is
  resolved by `FUN_004F2420`.
- They share **one skin object** at `character+0x98` (0x1A0 bytes, built from the model at `+0xB0` by `FUN_004D8430`);
  its `+0x3C` is compared against every part's `+0x04` -- most likely the shared **skeleton**. Parts are set per slot
  by `FUN_004D7F40(part, slot)`.
- `FUN_006F52C0(skin, character+0xAC)` binds the skin to the character's **animation/pose** (`+0xAC`); then the
  `FirstPerson.SkinAttachment` / `ThirdPerson.SkinAttachment` event goes out (`FUN_004F2F40`) -- presumably what the
  weapon attaches to (a bone of that skeleton), i.e. the weapon is its own model.
- **Skinning is on the GPU**: the skinned shaders take `g_axBoneMatrices` (a bone-matrix array in a constant buffer).
  Bone control can therefore sit at the CPU pose / matrix palette before upload (data writes, CEG-safe) or at the
  uploaded constant buffer per draw (the MOHW route).
- No bone names in the exe (data-driven, like the animation names). Dismemberment part names (`arm_left`,
  `Upper Right Arm`, ...) at `009D2240..`.
**Live (2026-10-17, user + `src/hooks/mesh_probe.cpp`):**
- The mesh-queue pass (`0053AB20`, return `00808E14`) draws the player's whole body and gun. All its skinned draws
  use one vertex shader and one bone buffer, **VS constant buffer slot 5, 6288 bytes, dynamic**.
- **The first-person body is one model** (one index / vertex buffer pair, 15 submeshes sharing one palette fill):
  user-found draw #7 = arms (1525 indices), #8 = hands (4461 indices). **The weapon is its own model** with its own
  palette fill (the pistol: draws #22-31, one index buffer). Draw numbers shift with state sorting; the buffers and
  index counts identify the parts.
- **Bone upload** (`FUN_00677E60`, the skinned-mesh draw): `Map(cb5, WRITE_DISCARD)` (returns to `00677F83`), then
  `FUN_00676D70` writes the header and `FUN_00675F30` the bones, `Unmap`, `VSSetConstantBuffers(5, 1, cb5)`, draws.
  Layout: `+0x00` the model's world matrix (4x4 row-major, translation in row 3), `+0x40` world * view * projection,
  `+0x80` float4 (near, 1/(far - near), 0, 0 on one path), **`+0x90` 128 bones**, each the source's row-major 4x4
  stored transposed as 3 float4 (columns 0-2 as rows, translation in w); (6288 - 144) / 48 = 128 exactly.
- Each bone's slot [row 2].z is overwritten with `1 - FUN_004D3390(bone)`: `FUN_004D3390` = thiscall(obj, i) returning
  `obj+0x15C[i]` (count `obj+0x160`) -- a **per-bone factor** in what is likely the skin object (0x1A0 bytes, so
  +0x15C fits), presumably the dismemberment hide; the shader rebuilds that rotation component.
- So there are two control points: per draw, rewrite the mapped bone buffer for the first-person body / weapon (render
  only: whole-model moves via the header's world and WVP matrices, per-bone moves via the bones); or the per-bone
  factor in heap memory (hide bones). The CPU source of the bone matrices is `FUN_00675F30`'s second argument.
- **Marine first-person bones** (user-found with the mod's bone hide, 1-based): left arm to the wrist 6-9, left hand
  10-25, right arm to the wrist 28-31, right hand 32-47. The weapon model follows the right controller when its
  world (and WVP = W' * W^-1 * WVP) is rewritten (verified 2026-10-17).
- **Marine bone map** (0-based, from the vertex read-back; model y is down, x- = left; parts 7 = arms, 8 = hands):
  5 head / neck (x0, highest), 6 left clavicle (torso parts only), 7 upper arm, 8 forearm, 9 hand, 10-24 fingers,
  25 forearm twist (parts 7 8), 26 upper arm twist, 27 left shoulder (torso parts 11 13 only); right 28-49 mirror
  it (28 clavicle ... 47 forearm twist, 48 upper arm twist, 49 shoulder). Torso / legs by centroid height (model y,
  feet ~0, head ~-1.65): 1 pelvis (-1.12), 2 stomach (-1.24), 3 spine (-1.33), 4 chest (-1.54), 50 top (-1.72),
  51 / 52 front pouches at belly height, 53-56 left leg (thigh, calf, foot, toe), 57-60 right leg, 61 belt front.
  The idle animation holds the left shoulder well forward; the mod's torso turn (from the stomach; a separate chest
  turn was tried and dropped as unneeded) turns bones 2-52 (not the hands) about the vertical before the arm IK, and
  an elbow swivel turns the IK's elbow about the shoulder-wrist line (+ from the arm's outward side toward down; the
  forearm-continuation hint alone leaves the elbows a little high and out). The twist bones sit
  outside the 1-based hide ranges above: an arm move that leaves them out stretches the wrist and upper arm.
- **Marine body vertex** (stride 64, read back 2026-10-17): +0 position float3, +12 blend indices as 4 bytes (live:
  `57 1 0 0` with weights 0.94 / 0.06; +16 repeats the first bytes, purpose unknown), +20 blend weights as 4 x SHORT4N (`FF 7F 00 00 00 00 00 00` = 1, 0, 0, 0), +28 / +36 / +44 normal, tangent,
  binormal as half4, +52 two half2 UVs, +60 a float (1.0). Weight detection that only tried bytes missed it, so the
  first arm build never had hand centres.
- `src/hooks/first_person.cpp`: the bone buffer is re-uploaded before a first-person model's first draw (Map
  WRITE_DISCARD) with: bones collapsed (hide); each hand moved rigidly so its centre (the hand bones' bind-space
  centroids from a vertex read-back, under their current matrices) sits on its controller, turned by camera ->
  controller; then two-bone IK on the upper arm and forearm. Joints come from the same read-back: the centre of the
  vertices skinned across a joint's two segments (shoulder = torso / upper arm, elbow = upper arm / forearm, wrist =
  forearm / hand), taken through the game's current matrices each frame, so the segment lengths are the game's. The
  shoulder stays (follows the hand only past full reach), the elbow sits on the reach circle on the side where the
  forearm would continue straight back from the hand, the hinge frame (segment axis + bend normal) gives each
  segment's rotation, and the forearm twist bone takes half of the hand's roll about the forearm. Measured
  2026-10-17: the Marine's joints sit at shoulder / elbow / wrist x = 0.18 / 0.42 / 0.62 (bind), upper arm 0.26-0.28,
  forearm 0.23-0.24 (shoulder to wrist ~0.50, shorter than a player's reach). Stretching the segments (tried as an
  arm-length setting, removed) pulls the mesh out of line with the hands; the body offset (setting) instead moves the
  whole body model's world and WVP along its own level forward / up, bringing the shoulders under the head. **The
  body model's frame** (after the bones, before the world matrix): y = down, z = sideways (an offset along z moved
  the body sideways), x = forward or back (the mod settles the sign against the view direction). It **does not pitch
  with the view** (stayed level while the view went 0 to -60 deg); any look up / down posing would be in the bones
  (unchecked). The weapon's world
  matrix is carried from the camera's frame to the controller's. Bone layout per slot: the source's row-major 4x4 S
  stored as columns 0-2, S[2][2] rebuilt as (row0 x row1).z since its slot holds the per-bone factor.
- The character object the look controller passes to SetLookDirection is NOT the one holding the skin attachment
  fields (`+0x98` etc. read as floats there).

---

## 10. Steam CEG integrity checks (2026-10-16, live-confirmed)

The exe is not packed, but it carries **Steam CEG** tamper checks. With 54 inline (MinHook) hooks on game functions,
the game exits cleanly (exit code 0, no exception) about 0.6 s into the first level load, both times. The exit hook
(kernel32 `ExitProcess`, `avpvr_crash.log`) caught it: `ExitProcess(0)` from the `AVP_ConnectionState_Loading::PerformLoad`
thread, fake return address `0x8000DEAD`, stack wiped.

- Kill stubs: `FUN_0083A1C0`, `FUN_0083A2B0`, `FUN_0083A3A0`, plus unnamed ones at `0083A79F`..`0083AFEF` — each zeroes
  the stack, then jumps to `ExitProcess` through an obfuscated pointer (`DAT_0096E17C + DAT_0096E168`, ...).
- Callers (the checkers): `FUN_0083B0E0`, `FUN_0083B140` (and `FUN_0083A5C0`).
- A separate exit site at `00876FF0` (`call 00877CE0; push 0x8000DEAD; call [ExitProcess]`) uses 0x8000DEAD as the
  exit *code* — not the path seen live.

**Consequence:** no inline patches on CEG-checked game code. Use vtable-slot swaps (camera component, look controller,
entity tick are virtual), IAT hooks (XInput, kernel32), hooks inside system DLLs (swapchain Present in dxgi), direct
memory reads/writes of the camera structs, and hardware breakpoints for the few non-virtual functions.

**`.rdata` is checked too (live, 2026-10-16):** swapping only the five `CamComp_ApplyToRenderCamera` vtable slots in
`.rdata` (no code bytes changed, the hook never even ran) produced the same silent exit at the first level load.
So vtable-slot and exe-IAT hooks are out as well. **Virtual functions are hooked with per-object shadow vtables**
(`src/hooks/vmt_shadow.cpp`): a copy of the class vtable in mod memory with stub entries, and a heap scan that
validates each object (camera component: the constructor's fixed `.rdata` pointers `+0xA8 = 0091D4B4`,
`+0xE0 = 0091D4BC` and a settings pointer at `+0x7C`; the first try, an owner back-pointer, was wrong) before atomically repointing
its vtable pointer. Only heap memory is written.

---

## 11. Stereo: native multi-view ruled out, render-twice works (2026-10-16, live)

**Native multi-view is dead.** With `g_ViewCount = 2`, draw counts don't double. `RenderQueue_SubmitMeshPerView`
submits opaque meshes (alpha 1.0, most of the scene) as one state-sorted packet with all view bits, executed once;
only transparent objects and lights get per-view packets. So view 1 got only the light pass (35-70 draws), which
left view 0 unlit. Making opaque geometry per view would need code changes (CEG).

**Frame structure** (in-game state function at `00812330`): `Frame_BeginFrame(1)` (`00808DA0`),
`Frame_RenderScene()` (`0080A440` -> `Scene_RenderFrame(*0x00A71C50)` at `00808E00`), `Frame_EndFrame` -> present.
`Scene_RenderFrame` is ~30 subsystem passes; the mesh queue (`Scene_MeshQueuePass` `0053AB20`: open queue, traverse,
`RenderQueue_SortAndExecute`) is ~55% of the draws.

**Render-twice works:** from the swapchain Present hook, call `Frame_BeginFrame(1)` + `Frame_RenderScene()` again with
a moved camera. Two full renders per frame at 60 fps (4K, vsync); draws and viewport calls double. Requirements found:
- Only on gameplay frames (gate: the player camera component's shadow hook fired since the last Present). During the
  level load the second render hung the main loop.
- Skip the per-frame job batch join for the second render: `Scene_JoinJobBatchAndPost` (`004F9220`) ->
  `JobBatch_Join` (`004F6B90`) waits on one event per worker when `g_JobBatchKickFrame` (`009CCD0C`) ==
  `g_FrameCounter` (`009E1480`); the events were consumed by the first render -> infinite wait. Moving the kick frame
  aside for the second render (data write) fixes it.
- Camera motion blur must be off (it compares each render with the previous one -> the whole world smears).
- **Between frames, view 0 must hold the game's own camera** (2026-10-17): game logic reads it before the next camera
  apply. A wide head-centred camera left there (first builds) made the Predator's "look at" objectives never complete in
  VR; with the game's camera back (and head aim pointing it where the head looks) they do, with no pop-in.
- Culling is reused from the first render (per-frame visibility caches keyed by `g_FrameCounter`): no visible gaps
  even with a 30-degree yaw on the second render.
- **Leave the D3D lock after the second render (2026-10-17).** `Render_BeginFrame` -> `FUN_0066B070` enters the
  critical section `g_D3DLock` (`00A257FC`) that guards the device; only `D3D_PresentFrame` leaves it, after Present.
  The right eye's `Frame_BeginFrame` runs inside Present, so without one extra `LeaveCriticalSection` the main thread
  kept the lock with a recursion count = stereo frames rendered. Re-entrant, so the main thread never noticed, but
  the level loader (another thread, creating the next level's GPU resources) waited on it forever: every load after
  the first of a session sat on an animating loading screen. `avpvr_stereo.log` prints the recursion count (1 =
  balanced: the game's own, left after Present).
- Projection: `Cam_BuildProjection` (`004FA900`) takes the horizontal scale from the global `g_ProjectionAspect`
  (`009CCD54`), not `cam+0xE4`, so each eye writes its aspect there (and the game's value goes back on flat frames).

---

## 12. HUD: the GUI pass, the VR HUD panel, the motion tracker (2026-10-16/17, live)

**The HUD ignores the view's viewport rect** (`cam+0x3C`). Its only readers seen live: `Gui_MapRectToViewViewport`
for text drawn with flag 0x10, and `Render2D_FadeQuadInViewport` (`00656A80`, a colour quad -- the "flashing black
box" when the rect was squeezed).

**The HUD is the GUI pass**, `Gui_RenderPass` (`0079C480`, -> `Gui_RenderScreens` `00627EA0`), the last call of
`Scene_RenderFrame` (`00808F33`, returns to `00808F38`). It draws into the scene target with the engine's cached
full-screen viewport and sets no D3D viewport of its own -- so a squeeze can't hang off `RSSetViewports` inside it.

**Pass detection at draw time** (`src/hooks/d3d_probe.cpp`, table in `src/hooks/hud_passes.h`): `Frame_RenderScene`'s
return address `0080A44B` sits at a fixed stack slot A for each of the two renders (the game's, and the right eye's
from the Present hook, deeper). A is learned once per render by a stack scan (game render: the shallowest match,
since stale copies of the right eye's frame lie deeper; right eye: the deepest). After that, the running
`Scene_RenderFrame` call is read from A-4 (calls before `00808E93`'s `PUSH EDI`), A-8 (later argument-less calls),
A-12 (the GUI call: `PUSH 0` -> `[A-8] = 0`, `[A-12] = 00808F38`) or A-16 (`00808F0B` / `00808F14`, after
`PUSH ESI`): three to five dword reads per draw.

**VR HUD panel:** from the first GUI-pass draw to the eye's copy, the game's full-screen viewport (and scissors) are
squeezed into a 16:9 panel while the bound target is the scene target / back buffer; offscreen targets keep theirs.
Size, distance (per-eye convergence: centre tan = -eyeOffset / distance) and height are dashboard settings.

**Late passes in gameplay** (draw counts per pass, 2026-10-17): only `005FF120` (the whole 3D scene) and the GUI pass
draw; squeezing from `005FF120` shrank the world. Passes the user identified by hiding them: all lighting, prop
meshes, floor and wall meshes (among `0053AB20` / `005D7C50` / `005D2900` / ...; not yet tied to addresses).

**Marine motion tracker:** `Scene_RenderFrame`'s tail jump (`00808F4B`, when `009CFDED` != 0) -> `Hud_MarineTrackerTail`
(`006EF6A0`, only for `g_PlayerSpecies` (`009D4BA0`) == 2) -> `Hud_DrawMotionTracker` (`006C8810`) queues two quads
into the 2D batch, flushed by `Render2D_FlushBatch` (`00675560`) at the start of `Frame_EndFrame`: an 18-index draw
with a 512x512 texture (format 77: grid clock, wave, blips) and a 6-index draw with a 128x128 one (the objective
arrow). They run outside every listed pass. A **second, live copy** of the tracker -- no objective arrow, its clock
vanishing whenever the wave or a blip shows -- is some other draw sampling the 512x512 texture, placed for the flat
screen; it stays at its full-screen spot. Hiding every other draw that samples the HUD tracker's textures removes
exactly it (user-verified): `HideDuplicateTracker`, bound textures tracked in a `PSSetShaderResources` hook.
Not drawn by: other threads, command lists, indirect draws (all zero in gameplay).

**Pause / menus:** `Game_IsPausedWithMenu` (`0080A450`) is true for the in-game pause menu; VR shows those frames
(and cinematics) on the flat virtual screen, with the game's own aspect.
