#pragma once
// Per-frame VR presentation. Two modes, chosen per frame:
//
//  STEREO (gameplay frames: the player camera component's ApplyToRenderCamera ran this frame, no cutscene)
//    1. Camera apply post-hook (shadow vtable replacement, main thread, before the scene render): WaitGetPoses, keep
//       the game's camera as the "base", write the LEFT eye camera into view 0. The game then renders the left eye.
//    2. Present hook: copy the finished frame (backbuffer) to the left eye texture, write the RIGHT eye camera, run
//       the scene render again (Frame_BeginFrame + Frame_RenderScene, job-batch join skipped), copy the scene target
//       to the right eye texture, restore the base camera, submit both eyes.
//    Eye camera = base yaw + head yaw, head pitch, head roll (engine angle convention, engine's own quaternion math),
//    eye position = base position + camera right axis * half-IPD. Projection: symmetric FOV covering the eye's
//    asymmetric frustum, cropped to the exact frustum with Submit texture bounds.
//
//  FLAT (everything else: menus, FMV movies, loading screens, cutscenes)
//    The game's normal frame is shown on a world-locked virtual screen (SteamVR overlay); black is submitted.
//
// Motion blur must be off in the game's graphics options (it compares each render with the previous one).

struct IDXGISwapChain;

namespace avp::stereo {

// Registers the camera-apply replacement with the shadow-vtable module. Call before StartVmtShadow().
void Register();

// From the Present hook, before the real Present. May lower the sync interval (VR paces the frame instead).
void OnPresent(IDXGISwapChain* swapChain, unsigned& syncInterval);

// While an eye renders: the HUD panel inside the eye image (scaleX, scaleY, offsetX, offsetY of the full render);
// nullptr otherwise. The D3D hooks squeeze the game's GUI pass (the HUD) into it.
const float* HudPanel();

// Main thread: an eye is being rendered (left: from the camera apply to its copy at Present; right: its render).
bool InEyeRender();

// A stereo frame started within the last half second (VR gameplay is running).
bool StereoActive();

// The last head pose's yaw (relative to the recenter reference, + = turned left), pitch (+ = up) and roll, degrees,
// in OpenVR's convention (no engine signs). False before the first valid pose.
bool HeadAnglesDeg(float out[3]);

// Main thread: the latest head pose's rotation in the engine's convention, relative to the recenter reference
// (3x3 row-major, columns = right, up, forward). False before the first valid pose / recenter.
bool HeadRotation(float m[9]);

// The engine's rotation for (pitch, yaw, roll) radians, its own conventions (+ pitch = up, + yaw = right):
// 3x3 row-major, columns = right, up, forward.
void EngineEulerMatrix(const float angles[3], float m[9]);

// Recenters done so far (each resets the head yaw reference).
unsigned RecenterCount();

// The thread that presents (diagnostics).
unsigned long PresentThread();

// A hand pose from vr/vr_input.h (OpenVR tracking space) in the game's world: relative to the head, through the
// player frame, from the game camera's position. rot = 3x3 row-major, columns = right, down, forward (the engine's
// convention); pos = world position. False without a player frame (head aim off) or head pose.
bool HandToWorld(const float pose[3][4], float rot[9], float pos[3]);

// The inverse for points: a game world position in OpenVR tracking space (metres), as HandToWorld maps them (the head
// at the game camera's position). head (optional) = the head's position in tracking space. Main thread (the frame's
// head pose); false as HandToWorld.
bool WorldToTracking(const float world[3], float tracking[3], float head[3] = nullptr);

// The game's own camera this frame (as its camera apply left it): rotation (columns right, down, forward), position.
void GameCamera(float rot[9], float pos[3]);

// Any thread: the next stereo frame takes the current head yaw as straight ahead.
void RequestRecenter();

} // namespace avp::stereo
