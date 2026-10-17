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

// Any thread: the next stereo frame takes the current head yaw as straight ahead.
void RequestRecenter();

} // namespace avp::stereo
