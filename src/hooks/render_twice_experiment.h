#pragma once
// EXPERIMENT: can the engine render the 3D frame a second time per frame (the stereo strategy that replaces native
// multi-view, which is ruled out -- docs/re/FINDINGS.md)?
//
// From the swapchain Present hook (d3d_probe.cpp, inside dxgi.dll -- CEG-safe), after the game's normal frame has been
// copied to the backbuffer: save that backbuffer, yaw view 0's camera by kYawDegrees, run the in-game frame's render
// calls again -- FUN_00808DA0(1) (begin-frame, clears) and FUN_0080A440() (scene: FUN_00808E00 with the global game
// object) -- then show the normal frame's left half next to the second render's right half, restore the camera and
// let Present continue. Calling game functions changes no bytes, so CEG doesn't object.
//
// What to look for: a right half turned 30 degrees, frame rate, flicker, doubled particle/animation speed (= state
// the scene render advances), HUD drawn twice. Log: avpvr_render2.log.

struct IDXGISwapChain;

namespace avp {

// Attaches the gameplay gate (pre-callback on the player camera's shadow hook). Call after the trace hooks.
void StartRenderTwiceExperiment();

// Called by the Present hook before the real Present. No-op unless dev::kRenderTwiceExperiment.
void RenderTwiceOnPresent(IDXGISwapChain* swapChain);

} // namespace avp
