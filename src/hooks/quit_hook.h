#pragma once
// Clean SteamVR shutdown when the game quits (2026-10-17): without it the process just died with its eye textures
// still submitted, and on a Steam Link connection the driver's "HMD graphics context flush & release" stalled
// ~460 ms, the stream reset unrecoverably and SteamVR asked to restart.
//
// Hooks PostQuitMessage (the game's window procedure, 00642B8F) and DestroyWindow of the game window (FUN_00642170)
// inside user32.dll -- a system DLL, CEG-safe -- and on the first of them calls vr::ShutdownForExit().

namespace avp {

// Once at startup (after MH_Initialize).
void InstallQuitHook();

// The game window (the swapchain's output window), noted by the Present hook while the swapchain is alive: the
// DestroyWindow hook only compares handles (2026-10-17: asking the swapchain from DestroyWindow at quit -- possibly
// already released -- hung the game's exit).
void SetGameWindow(void* window);

} // namespace avp
