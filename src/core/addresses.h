#pragma once
// Addresses in AvP_DX11.exe (build stamp 0x4C34AC5D, SizeOfImage 0x78D000). The exe has no ASLR
// (DllCharacteristics 0), so it always loads at 0x00400000 and these are absolute.
//
// Names match docs/re/avp_symbols.csv and the AVP2010 Ghidra project. Keep the three in sync: add a row to the CSV,
// run tools/ghidra/ApplySymbols.java, then add the constant here. Roles and confidence are in docs/re/FUNCTIONS.md.

#include <cstddef>
#include <cstdint>

namespace avp::addr {

// ---- Build identity ----
constexpr uintptr_t kImageBase      = 0x00400000;
constexpr uint32_t  kTimeDateStamp  = 0x4C34AC5D;
constexpr uint32_t  kSizeOfImage    = 0x0078D000;

// ---- Camera (render camera struct, setters, drivers) ----
constexpr uintptr_t Cam_SetPosition              = 0x004FA050;
constexpr uintptr_t Cam_SetRotationMatrix        = 0x004FA170;
constexpr uintptr_t Cam_SetRotationQuat          = 0x004FA2A0;
constexpr uintptr_t Cam_SetHorizontalFov         = 0x004FA3E0;
constexpr uintptr_t Cam_SetVerticalFov           = 0x004FA560;
constexpr uintptr_t Cam_RecomputeFrustum         = 0x004F9590;
constexpr uintptr_t Cam_InitDefaults             = 0x004FA6E0;
constexpr uintptr_t Cam_SetClipE0                = 0x004F9340;
constexpr uintptr_t Cam_SetClipDC                = 0x004F9320;
constexpr uintptr_t Cam_SetViewportRect          = 0x004F9360;
constexpr uintptr_t Cam_FlyCamUpdate             = 0x004FB980;
constexpr uintptr_t CamComp_ApplyToRenderCamera  = 0x00695760;
constexpr uintptr_t CamComp_UpdatePosition       = 0x00695D10;
constexpr uintptr_t CamComp_SetMode              = 0x00697330;
constexpr uintptr_t Entity_TickCameraComponent   = 0x006FF350;
constexpr uintptr_t Cinematic_ApplyCamera        = 0x0052A160;
constexpr uintptr_t Cinematic_SetClipPlanes      = 0x00528220;
constexpr uintptr_t CamShake_ApplyAllViews       = 0x0063E6B0;
constexpr uintptr_t Cam_CopyFromObject           = 0x006F54D0;

// ---- Render / D3D ----
constexpr uintptr_t Render_SetupViewportAspect   = 0x005D8790;
constexpr uintptr_t Render_BeginFrame            = 0x005D8B60;
constexpr uintptr_t Render_EndFrame              = 0x005D8C00;
constexpr uintptr_t Render_FinishAndPresent      = 0x0067BF00;
constexpr uintptr_t D3D_PresentFrame             = 0x0066A840;
constexpr uintptr_t D3D_CreateDeviceAndSwapChain = 0x0066C060;
constexpr uintptr_t D3D_HandleModeChange         = 0x0066C580;
// In-game frame (state function at 00812330): Frame_BeginFrame(1); Frame_RenderScene(); end-frame/present.
constexpr uintptr_t Frame_BeginFrame             = 0x00808DA0; // cdecl(char clear): view 0 current, render states
constexpr uintptr_t Frame_RenderScene            = 0x0080A440; // cdecl(): Scene_RenderFrame(*(0x00A71C50))
constexpr uintptr_t Scene_RenderFrame            = 0x00808E00; // the whole 3D frame: ~30 subsystem passes
constexpr uintptr_t Frame_EndFrame               = 0x0080A0E0; // -> Render_EndFrame -> present
// cdecl() -> bool: (paused 009E14A8 || time scale 009E1484 <= 0) && menu up (FUN_0079C4D0) && !00A71E27.
// Scene_RenderFrame skips a player draw step when it's true. VR: in-game menu frames go to the flat screen.
constexpr uintptr_t Game_IsPausedWithMenu        = 0x0080A450;

// ---- Multi-view ----
constexpr uintptr_t View_ForEachUpdateObjects    = 0x00662780;
constexpr uintptr_t View_ComputeVisibilityMask   = 0x006006A0;
constexpr uintptr_t View_ComputeBoundsVisibility = 0x005FE180;
constexpr uintptr_t View_SelectAndSortKey        = 0x005D8CA0;
constexpr uintptr_t RenderQueue_SubmitMeshPerView= 0x005D90B0;
constexpr uintptr_t RenderQueue_SubmitPerView    = 0x005D8FD0;
constexpr uintptr_t Mesh_SubmitForViews          = 0x004E8F50;
constexpr uintptr_t Decal_SubmitForViews         = 0x006E6260;
constexpr uintptr_t ShadowOrReflect_RenderPerView= 0x0075CAC0;
constexpr uintptr_t Gui_MapRectToViewViewport    = 0x005D3F80;

// ---- Look / aim ----
constexpr uintptr_t LookCtrl_Update              = 0x0069F650;
constexpr uintptr_t LookCtrlSub_Update           = 0x006ACBA0;
constexpr uintptr_t LookCtrl_UpdateLook          = 0x006ACE60;
constexpr uintptr_t LookCtrl_SetLookDirection    = 0x006AC1E0;
constexpr uintptr_t LookCtrl_GetAction           = 0x0069EBB0;
constexpr uintptr_t LookCtrlSub_HandleActions678 = 0x006AC740;

// ---- Input ----
constexpr uintptr_t Input_GetActionValue         = 0x0060DED0;
constexpr uintptr_t Input_ConsumeAction          = 0x0060DF40;
constexpr uintptr_t Input_PackActionBits         = 0x006256A0;
constexpr uintptr_t XInput_PollPad               = 0x00645AA0;
constexpr uintptr_t XInput_SetRumble             = 0x00659FB0;

// ---- Character ----
constexpr uintptr_t Char_SelectSkinAttachment    = 0x006A6450;
constexpr uintptr_t Entity_CreateVisionAndWeaponComps = 0x007062C0;

// ---- Console ----
constexpr uintptr_t Console_Tick                 = 0x0058CE80;
constexpr uintptr_t Console_Show                 = 0x0058D6B0;
constexpr uintptr_t Console_Close                = 0x0058C180;
constexpr uintptr_t Console_IsAllowed            = 0x0079D4E0;
constexpr uintptr_t Console_Draw                 = 0x0058D2F0;

// ---- Vtable slots (in .rdata) for slot hooks -- CEG forbids inline hooks on game code ----
// CamComp_ApplyToRenderCamera at +0x08 of the five camera-component vtables 0091D694, 00920054, 00921360, 009226E4,
// 00923280 (each verified to hold 0x00695760 in the shipping exe).
constexpr uintptr_t vt_CamComp_Apply_Slots[5] = {0x0091D69C, 0x0092005C, 0x00921368, 0x009226EC, 0x00923288};

// ---- Globals ----
constexpr uintptr_t g_CurViewCamera        = 0x009CCD48; // pointer
constexpr uintptr_t g_ViewCount            = 0x009CCD4C; // int, 1 in the exe
constexpr uintptr_t g_CurViewIndex         = 0x009CCD50; // int
constexpr uintptr_t g_CameraDirty          = 0x009CCD58; // byte
constexpr uintptr_t g_CurViewMask          = 0x009DB190; // byte
constexpr uintptr_t g_ViewCameras          = 0x009DB198; // ViewCamera[4]
constexpr uintptr_t g_CinematicActive      = 0x009EA24D; // byte
constexpr uintptr_t g_RenderOnlyCurrentView= 0x009D9008; // byte
constexpr uintptr_t g_D3DDevice            = 0x00A2581C; // ID3D11Device*
constexpr uintptr_t g_D3DContext           = 0x00A25820; // ID3D11DeviceContext*
constexpr uintptr_t g_SwapChain            = 0x00A25824; // IDXGISwapChain*
constexpr uintptr_t g_SceneTarget          = 0x00A25828; // ID3D11Resource*, copied to the backbuffer at present
constexpr uintptr_t g_BackBuffer           = 0x00A2582C; // ID3D11Resource*
constexpr uintptr_t g_BackBufferWidth      = 0x00A25814; // int
constexpr uintptr_t g_BackBufferHeight     = 0x00A25818; // int
constexpr uintptr_t g_ScreenWidth          = 0x009FAAFC; // int
constexpr uintptr_t g_ScreenHeight         = 0x009FAB00; // int
constexpr uintptr_t g_VSync                = 0x009CFB69; // byte
constexpr uintptr_t g_FrameDt              = 0x009E1494; // float
constexpr uintptr_t g_FrameCounter         = 0x009E1480; // int, keys per-frame caches (visibility masks, job joins)
// Per-frame job batch joined inside Scene_RenderFrame (FUN_004F9220 -> FUN_004F6B90): the render waits on one event
// per worker only if the batch was kicked this frame (kick frame == g_FrameCounter). A second render must not re-join
// (events already consumed -> infinite wait), so it moves the kick frame aside for its duration.
constexpr uintptr_t g_JobBatchKickFrame    = 0x009CCD0C; // int
constexpr uintptr_t g_JobBatchWorkerCount  = 0x009CCD10; // int
constexpr uintptr_t g_PlayerProfile        = 0x00A71E14; // pointer
// CRITICAL_SECTION around the D3D device: entered by Render_BeginFrame (FUN_0066B070), left by D3D_PresentFrame after
// Present. A second Frame_BeginFrame in the same frame (the VR right eye) must leave it once more, or the main thread
// keeps it forever (recursion count = stereo frames) and the level loader, which needs it for the new level's GPU
// resources, waits on it for good: 2026-10-17, every level load after the first of a session hung on the loading screen.
constexpr uintptr_t g_D3DLock              = 0x00A257FC;
// Player species index; 2 runs the motion tracker (Scene_RenderFrame's tail call FUN_006EF6A0 -> FUN_006C8810), so the
// marine. Others not verified.
constexpr uintptr_t g_PlayerSpecies        = 0x009D4BA0; // int
constexpr uintptr_t g_Console              = 0x009E77A0; // pointer
constexpr uintptr_t g_ConsoleGateA         = 0x009E1C58; // byte, must be != 0 for the console
constexpr uintptr_t g_ConsoleGateB         = 0x009E15C8; // byte, must be 0
constexpr uintptr_t g_ConsoleGateC         = 0x009FFD38; // dword, must be 0
constexpr uintptr_t cvar_FlyCam            = 0x00A66534; // bool
constexpr uintptr_t cvar_FixCam            = 0x00A66535; // bool
constexpr uintptr_t cvar_CameraUpdate      = 0x009D2828; // bool
constexpr uintptr_t cvar_RenderEverything  = 0x009FF31B; // bool

} // namespace avp::addr

namespace avp {

// One entry of g_ViewCameras (stride 0x15C). Only the fields identified so far; offsets are asserted below.
#pragma pack(push, 1)
struct ViewCamera
{
    float    pos[3];             // +0x00
    float    rot[9];             // +0x0C  3x3 rotation
    float    pitch, yaw, roll;   // +0x30  derived by Cam_SetRotationMatrix
    float    viewport[4];        // +0x3C  scaleX, scaleY, offsetX, offsetY (normalized; default 1,1,0,0)
    uint8_t  _pad4C[0xDC - 0x4C];
    float    clipDC;             // +0xDC  probably far
    float    clipE0;             // +0xE0  probably near
    float    aspect;             // +0xE4
    float    invAspect;          // +0xE8
    float    hHalfFov;           // +0xEC  radians
    float    hInvCos, hTan, hInvTan; // +0xF0
    float    vHalfFov;           // +0xFC  radians
    float    vInvCos, vTan, vInvTan; // +0x100
    uint8_t  _pad10C[0x114 - 0x10C];
    uint8_t  active;             // +0x114 view is rendered
    uint8_t  _pad115[0x15C - 0x115];
};
#pragma pack(pop)
static_assert(sizeof(ViewCamera) == 0x15C, "ViewCamera stride");
static_assert(offsetof(ViewCamera, viewport) == 0x3C, "viewport");
static_assert(offsetof(ViewCamera, hHalfFov) == 0xEC, "hHalfFov");
static_assert(offsetof(ViewCamera, vHalfFov) == 0xFC, "vHalfFov");
static_assert(offsetof(ViewCamera, active) == 0x114, "active");

constexpr int kMaxViews = 4; // the array holds exactly 4 (next global at +0x570)

inline ViewCamera* ViewCameras() { return reinterpret_cast<ViewCamera*>(addr::g_ViewCameras); }

template <typename T>
inline T& Global(uintptr_t address) { return *reinterpret_cast<T*>(address); }

} // namespace avp
