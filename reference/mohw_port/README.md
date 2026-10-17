# MOHW port reference (not compiled)

Copies of the Medal of Honor: Warfighter VR mod's generic VR pieces, taken 2026-10-16 from
`C:\Dev\VR Mod\Warfighter\Claude\Local Session`. They are **not part of the build**: every one of them reaches into
MOHW-specific code (its aiming controller, companion bridge, `mohw_offsets.h`, the MOHW settings table), so they get
ported into `src/` piece by piece as the AvP framework grows, with the MOHW couplings replaced by AvP equivalents.

| Folder / file | What it is | Port target |
|---|---|---|
| `openvr_direct/openvr_direct.*` | In-process 32-bit SteamVR: dynamic `openvr_api.dll` load, `VR_Init`, poses, per-eye submit | Goal 2/3 (stereo submit, head pose) |
| `openvr_direct/vr_input.*` | SteamVR Input: action manifest + **Oculus Touch binding profile embedded as JSON strings**, action → virtual pad mapping | Goal 4 (motion controls as gamepad) — rewrite actions for AvP |
| `openvr_direct/vr_overlay.*` | SteamVR dashboard settings tab (GDI-rendered menu, `SetOverlayRaw`) | Goal 1 (all settings in the overlay) |
| `openvr_direct/vr_hands.*` | Controller/hand pose helpers, calibration | Goal 4/5 |
| `openvr_direct/vr_steering.*` | MOHW driving wheel | Probably not needed |
| `sdk/settings.*`, `sdk/default_settings.h` | Settings table, ini persistence, dashboard menu definitions | Goal 1 — keep the table-driven design, AvP-specific names |
| `sdk/vr_math.h` | Vector/matrix/quaternion helpers (depends on `mohw_common.h`, trivial to cut) | Shared math |
| `sdk/logging.h` | Logger | Already ported as `src/core/log.h` |
| `hooks/xinput_hook.*` | Virtual XInput pad fed from VR input | Goal 4 — AvP's single `XInputGetState` caller is `XInput_PollPad` (`00645AA0`) |
| `hooks/present_hook.*` | Swapchain Present hook, SteamVR frame pump | Goal 2 — AvP's present is `D3D_PresentFrame` (`0066A840`) |
| `hooks/master_switch.*` | "Install nothing until a headset is detected" | Framework |
| `hooks/head_position.*`, `render_pose_stamp.*` | 6DoF head position, render-pose stamping for reprojection | Goal 3 |
| `hooks/laser_raycast.*` | Aim laser on real geometry | Goal 4 |

MOHW memory notes worth rereading before porting each piece: SteamVR 32-bit direct connection (load `openvr_api.dll`
dynamically, never link it statically), eye 1's submitted pose being ignored by SteamVR (submit pose pairs), VR
controls/bindings gotchas (autosaved SteamVR binding copies), dashboard overlay slot details.
