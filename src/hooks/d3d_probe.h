#pragma once
// D3D11 call counter: does the engine draw the scene once per frame, or once per view?
//
// Hooks (MinHook, inline) inside d3d11.dll / dxgi.dll -- system DLLs, not checked by Steam CEG -- reached through the
// game's own immediate context and swapchain vtables once they exist: Draw, DrawIndexed, DrawInstanced,
// DrawIndexedInstanced, RSSetViewports, and IDXGISwapChain::Present (frame boundary). Each draw is tagged with the
// engine's current view index (g_CurViewIndex) at the moment of the call. Once per second avpvr_d3d.log gets: frames,
// draws per frame (total / with view 0 current / view 1 current), viewport calls per frame, the view count in effect,
// and each distinct viewport rectangle the first time it is seen.

namespace avp {

// Starts a thread that waits for the game's device/context/swapchain globals, then installs the hooks.
void StartD3DProbe();

} // namespace avp
