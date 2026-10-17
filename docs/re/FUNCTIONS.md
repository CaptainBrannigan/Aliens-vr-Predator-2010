# AvP 2010 identified functions and globals

Generated from `avp_symbols.csv` (the source of truth; edit it, then re-run `ApplySymbols.java` to push names into Ghidra).
Program: AvP_DX11.exe, image base 00400000, addresses = x32dbg addresses. Confidence: high = role read directly from decompile; medium = inferred; open = role unknown.

84 functions, 37 globals/vtables. All are applied as names + plate comments in the AVP2010 Ghidra project.

## Camera

| Address | Kind | Name | Conf. | Role |
|---|---|---|---|---|
| `004F9320` | func | `Cam_SetClipDC` | medium | Write cam+0xDC (probably far clip) |
| `004F9340` | func | `Cam_SetClipE0` | medium | Write cam+0xE0 (probably near clip) |
| `004F9360` | func | `Cam_SetViewportRect` | high | Write 4 floats at cam+0x3C (viewport scale/offset) |
| `004F9590` | func | `Cam_RecomputeFrustum` | high | Build 5 frustum planes (cam+0x50..) from rotation + FOVs |
| `004FA050` | func | `Cam_SetPosition` | high | Set current view camera position (vec3 at cam+0x00), update plane distances |
| `004FA170` | func | `Cam_SetRotationMatrix` | high | Set 3x3 rotation (cam+0x0C), derive pitch/yaw/roll (cam+0x30..0x38), recompute frustum |
| `004FA2A0` | func | `Cam_SetRotationQuat` | high | Set rotation from quaternion (FlyCam path) |
| `004FA3E0` | func | `Cam_SetHorizontalFov` | high | (hFov, aspectScale): set horizontal half-FOV (cam+0xEC) and derive vertical (cam+0xFC) |
| `004FA560` | func | `Cam_SetVerticalFov` | high | (vFov, aspectScale): set vertical half-FOV (cam+0xFC) and derive horizontal |
| `004FA6E0` | func | `Cam_InitDefaults` | high | Camera defaults (FOV, clip, misc cam+0x110..0x158) |
| `004FB980` | func | `Cam_FlyCamUpdate` | high | AVP.FlyCam free-camera movement from input actions 0-10 |
| `00528220` | func | `Cinematic_SetClipPlanes` | medium | Sets cam+0xE0/+0xDC for cinematics |
| `0052A160` | func | `Cinematic_ApplyCamera` | high | Cutscene camera: pos +0x38, matrix +0x44, vFOV +0x9C; only when g_CinematicActive |
| `00617F15` | func | `Cam_RenderWithTempCamera` | medium | Saves camera, renders with a temporary camera (render-to-texture?), restores |
| `0063E6B0` | func | `CamShake_ApplyAllViews` | high | Per view: applies camera shake to rotation, drives rumble |
| `0063F360` | func | `Console_CameraShakeStart` | high | Console command Camera.Shake.Start |
| `0063F3E0` | func | `Console_RegisterCameraShake` | high | Registers Camera.Shake.Start/Stop |
| `00695670` | func | `CamComp_Dtor` | high | Camera component dtor, unregisters cvars |
| `00695760` | func | `CamComp_ApplyToRenderCamera` | high | Camera component vtable+0x08: computes pos/rot/FOV and calls Cam_* setters; FlyCam/FixCam branches |
| `00695D10` | func | `CamComp_UpdatePosition` | high | Camera component vtable+0x0C: spring/collision position smoothing; gated by AVP.Camera.Update |
| `00696D00` | func | `CamComp_Ctor` | high | Camera component ctor (vtable 0091D694), registers FlyCam/3rd-person cvars |
| `00697330` | func | `CamComp_SetMode` | medium | Sets camera mode 0/1/2 (cam comp +0x08) |
| `006973E0` | func | `CamComp_LoadSettings` | medium | Reads hashed camera settings (FOVs at +0x80..+0x94) |
| `006F54D0` | func | `Cam_CopyFromObject` | open | Copies pos+matrix from an object (caller FUN_006ef6e0) into the camera - source unknown |
| `006FF2A0` | func | `Cvar_RegisterCameraDebug` | high | Registers AVP.Camera.Update, AVP.Player.ShowPosition, ... |
| `006FF350` | func | `Entity_TickCameraComponent` | high | Per-entity tick: calls camera component +0x0C (if AVP.Camera.Update) then +0x08 |
| `00700EA0` | func | `Char_GetCameraClip` | medium | Returns a per-character clip value (vtable+0x84) used for Cam_SetClipE0 |
| `0091D694` | data | `vt_CameraComponent` | high | Camera component vtable |
| `009CCD48` | data | `g_CurViewCamera` | high | Pointer to current view camera struct |
| `009CCD58` | data | `g_CameraDirty` | high | Set by every camera setter |
| `009DB198` | data | `g_ViewCameras` | high | Array of view cameras, stride 0x15C |
| `009DB708` | data | `g_FlyCamInvertPitch` | medium | FlyCam pitch invert (toggled by FlyCam action 10); first global after the 4-slot view array |
| `009EA24D` | data | `g_CinematicActive` | medium | Cutscene playing (gates cinematic camera and player look) |

## Render

| Address | Kind | Name | Conf. | Role |
|---|---|---|---|---|
| `005D8790` | func | `Render_SetupViewportAspect` | medium | Viewport/letterbox/aspect from screen size; calls Cam_SetViewportRect + Cam_SetHorizontalFov |
| `005D8B60` | func | `Render_BeginFrame` | medium | Per-frame counters reset; dead half-width mode behind g_HalfWidthMode |
| `005D8C00` | func | `Render_EndFrame` | high | End of frame -> Render_FinishAndPresent |
| `00669920` | func | `D3D_GetSwapChain` | high | return g_SwapChain |
| `00669C10` | func | `D3D_EnumOutputModes` | medium | Enumerates display modes via output |
| `0066A840` | func | `D3D_PresentFrame` | high | context->CopyResource(g_BackBuffer, g_SceneTarget) then swapchain->Present(vsync,0) |
| `0066C060` | func | `D3D_CreateDeviceAndSwapChain` | high | D3D11CreateDeviceAndSwapChain; R8G8B8A8_UNORM_SRGB, 2 buffers |
| `0066C580` | func | `D3D_HandleModeChange` | medium | Fullscreen state query + resize |
| `0067BF00` | func | `Render_FinishAndPresent` | high | Final passes then D3D_PresentFrame |
| `009CFB69` | data | `g_VSync` | medium | Present sync interval flag |
| `009EA257` | data | `g_HalfWidthMode` | high | Dead flag (BSS, never written) for a half-width viewport mode |
| `009FAAFC` | data | `g_ScreenWidth` | medium | Screen width used for viewports |
| `009FAB00` | data | `g_ScreenHeight` | medium | Screen height used for viewports |
| `00A25814` | data | `g_BackBufferWidth` | high | Swapchain width |
| `00A25818` | data | `g_BackBufferHeight` | high | Swapchain height |
| `00A2581C` | data | `g_D3DDevice` | high | ID3D11Device* |
| `00A25820` | data | `g_D3DContext` | high | ID3D11DeviceContext* (immediate) |
| `00A25824` | data | `g_SwapChain` | high | IDXGISwapChain* |
| `00A25828` | data | `g_SceneTarget` | medium | Offscreen target copied to the backbuffer at present |
| `00A2582C` | data | `g_BackBuffer` | medium | Backbuffer resource (CopyResource destination) |

## Views

| Address | Kind | Name | Conf. | Role |
|---|---|---|---|---|
| `004E8F50` | func | `Mesh_SubmitForViews` | high | Mesh submit: builds view mask from active views then queues |
| `005D3F80` | func | `Gui_MapRectToViewViewport` | high | Maps a 2D rect/point into the current view's viewport: screen * (scale cam+0x3C/+0x40) + offset cam+0x44/+0x48 |
| `005D8CA0` | func | `View_SelectAndSortKey` | high | Switch current view + compute distance sort key |
| `005D8FD0` | func | `RenderQueue_SubmitPerView` | high | Emits one packet per view bit (simpler variant) |
| `005D90B0` | func | `RenderQueue_SubmitMeshPerView` | high | Emits one draw packet per view bit |
| `005FE180` | func | `View_ComputeBoundsVisibility` | high | Bounds vs every active view -> mask |
| `006006A0` | func | `View_ComputeVisibilityMask` | high | Per-object per-view visibility bitmask (checks view active byte cam+0x114) |
| `0063EB00` | func | `View_AnyWithinDistance` | high | True if a point is within range of any view camera |
| `00662780` | func | `View_ForEachUpdateObjects` | medium | Loops all views updating a per-view object list |
| `006E6260` | func | `Decal_SubmitForViews` | medium | Decal/projected submit per active view |
| `0075CAC0` | func | `ShadowOrReflect_RenderPerView` | medium | Renders something per view with g_RenderOnlyCurrentView set |
| `0075CC60` | func | `RenderObj_CtorPerViewArrays` | medium | Allocates arrays sized by g_ViewCount |
| `009CCD4C` | data | `g_ViewCount` | high | Number of views (1, static data, never written) |
| `009CCD50` | data | `g_CurViewIndex` | high | Current view index |
| `009D9008` | data | `g_RenderOnlyCurrentView` | high | When set, per-view loops only process the current view |
| `009DB190` | data | `g_CurViewMask` | high | 1 << current view |

## Look

| Address | Kind | Name | Conf. | Role |
|---|---|---|---|---|
| `00692B90` | func | `AnimSet_BindPlayerAnims` | high | Binds :AimDownToUp/:PrimaryWeaponShootBegin/... anim names into a table |
| `0069E570` | func | `LookCtrl_Ctor` | high | Look controller base ctor (vtable 0091FAB8); registers Controls.* sensitivity/aim-assist cvars |
| `0069EBB0` | func | `LookCtrl_GetAction` | high | (actionIdx, ?, consume): blocked during UI/cinematic, else Input_GetActionValue |
| `0069F650` | func | `LookCtrl_Update` | high | Base per-tick: turn speeds + aim assist, then vtable+0x60 |
| `006AC1E0` | func | `LookCtrl_SetLookDirection` | high | vtable+0x9C: stores look quat at ctrl+0x70, char vtable+0xB4 (set look), +0xBC (aim point raycast) |
| `006AC740` | func | `LookCtrlSub_HandleActions678` | medium | vtable+0xA4: handles actions 6/7/8 (hold timers) |
| `006ACBA0` | func | `LookCtrlSub_Update` | high | Subclass per-tick (vtable 00920D70 +0x08), calls LookCtrl_Update |
| `006ACE60` | func | `LookCtrl_UpdateLook` | high | vtable+0x60: actions 0-3 -> yaw/pitch, sensitivity, assist, pitch clamp, yaw wrap -> SetLookDirection |
| `0091FAB8` | data | `vt_LookCtrlBase` | high | Look controller base vtable |
| `00920D70` | data | `vt_LookCtrlSub` | high | Look controller subclass vtable (3 siblings: 009220F8, 00922E40, 00923C88) |
| `00A71E14` | data | `g_PlayerProfile` | medium | +0xB5 invert Y, +0xBA/+0xBB assist, +0xCC/+0xD0 sensitivity |

## Input

| Address | Kind | Name | Conf. | Role |
|---|---|---|---|---|
| `0060DED0` | func | `Input_GetActionValue` | high | return idx < count ? actions[idx] : 0 (float action array) |
| `0060DF40` | func | `Input_ConsumeAction` | high | Clears/consumes an action |
| `006256A0` | func | `Input_PackActionBits` | high | Packs actions 0..32 into a bitmask (net/demo input) |
| `00644F70` | func | `Input_ClassifyPadName` | high | Pad name -> type (XInput = 5) |
| `00645AA0` | func | `XInput_PollPad` | high | XInputGetState -> normalized sticks pad+0x248..0x254, buttons pad+0x258, triggers +0x260/+0x261 |
| `00659FB0` | func | `XInput_SetRumble` | high | XInputSetState rumble (called from camera shake) |

## Cvar

| Address | Kind | Name | Conf. | Role |
|---|---|---|---|---|
| `0058A870` | func | `Console_Print` | high | Prints a line to the console |
| `0058B7C0` | func | `Console_CommandExists` | medium | name -> bool |
| `0058B7E0` | func | `Cvar_Exists` | high | name -> bool |
| `0058C6C0` | func | `Cvar_Unregister` | high | name |
| `0058C840` | func | `Console_RegisterCommandB` | medium | Second command-registration variant |
| `0058C900` | func | `Console_RegisterCommand` | high | (name, fn, ?) |
| `0058C960` | func | `Cvar_RegisterBool` | high | (name, bool*, flags) |
| `0058C9E0` | func | `Cvar_RegisterFloat` | high | (name, float*, flags, default, ?) |
| `0060BFA0` | func | `Cvar_RegisterVisibility` | high | Visibility.RenderEverything/FreezeCameraNode/XRayMode |
| `009CFD74` | data | `cvar_FlyCamFov` | high | AVP.FlyCam.FOV |
| `009D2828` | data | `cvar_CameraUpdate` | high | AVP.Camera.Update |
| `009FF318` | data | `cvar_FreezeCameraNode` | high | Visibility.FreezeCameraNode |
| `009FF31A` | data | `cvar_XRayMode` | high | Visibility.XRayMode |
| `009FF31B` | data | `cvar_RenderEverything` | high | Visibility.RenderEverything |
| `00A66534` | data | `cvar_FlyCam` | high | AVP.FlyCam |
| `00A66535` | data | `cvar_FixCam` | high | AVP.FixCam |
| `00A66536` | data | `cvar_FlyCamUseFov` | high | AVP.FlyCam.UseFOV |

## Console

| Address | Kind | Name | Conf. | Role |
|---|---|---|---|---|
| `0058A320` | func | `Console_CloseCmd` | high | Close (command path) |
| `0058C180` | func | `Console_Close` | high | Closes console |
| `0058CA80` | func | `Console_Init` | high | Creates console object (g_Console), registers built-in commands |
| `0058CDF0` | func | `Console_Reset` | medium | Resets console state |
| `0058CE80` | func | `Console_Tick` | high | Opens console when Console_IsAllowed and a request is pending; per-frame update |
| `0058D160` | func | `Console_Ctor` | high | Console object ctor (vtable 008CBA10) |
| `0058D2F0` | func | `Console_Draw` | high | Draws console (consbk.dds) when slide height > 0 |
| `0058D6B0` | func | `Console_Show` | high | vtable+0x14: (show, instant) - slide open/closed |
| `0058D700` | func | `Console_AnimateSlide` | high | Slide animation |
| `0079D4E0` | func | `Console_IsAllowed` | high | DAT_009e1c58 != 0 && DAT_009e15c8 == 0 && DAT_009ffd38 == 0 |
| `008CBA10` | data | `vt_Console` | high | Console vtable |
| `009E15C8` | data | `g_ConsoleGateB` | medium | Must be zero for console (written indirectly) |
| `009E1C58` | data | `g_ConsoleGateA` | medium | Must be nonzero for console (written indirectly - live check) |
| `009E77A0` | data | `g_Console` | high | Console object pointer |

## Character

| Address | Kind | Name | Conf. | Role |
|---|---|---|---|---|
| `006A6450` | func | `Char_SelectSkinAttachment` | high | Chooses FirstPerson vs ThirdPerson.SkinAttachment by char+0xE8 bit 2 |
| `007062C0` | func | `Entity_CreateVisionAndWeaponComps` | medium | Creates VisionMode + Weapon components on an entity |

## Misc

| Address | Kind | Name | Conf. | Role |
|---|---|---|---|---|
| `0087992A` | func | `Audio_FormatName` | high | Audio format string (Mono/Stereo/...) - not graphics |
| `009E1494` | data | `g_FrameDt` | medium | Frame delta time (scales speeds everywhere) |
