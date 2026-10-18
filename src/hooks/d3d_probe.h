#pragma once
// D3D11 call counter: does the engine draw the scene once per frame, or once per view?
//
// Hooks (MinHook, inline) inside d3d11.dll / dxgi.dll -- system DLLs, not checked by Steam CEG -- reached through the
// game's own immediate context and swapchain vtables once they exist: Draw, DrawIndexed, DrawInstanced,
// DrawIndexedInstanced, RSSetViewports, and IDXGISwapChain::Present (frame boundary). Each draw is tagged with the
// engine's current view index (g_CurViewIndex) at the moment of the call. Once per second avpvr_d3d.log gets: frames,
// draws per frame (total / with view 0 current / view 1 current), viewport calls per frame, the view count in effect,
// and each distinct viewport rectangle the first time it is seen.

//
// VR HUD panel: from the first draw of the game's GUI pass -- the last call in Scene_RenderFrame, FUN_0079C480 ->
// FUN_00627EA0, which draws the HUD screens -- full-screen viewports (and scissors) are squeezed into
// stereo::HudPanel() while an eye renders (see d3d_probe.cpp for how the pass is recognised).

struct ID3D11DeviceContext;

namespace avp {

// Starts a thread that waits for the game's device/context/swapchain globals, then installs the hooks.
void StartD3DProbe();

// Puts back the GUI pass's own (unsqueezed) viewport/scissor if the last one set was squeezed, so later passes that
// rely on the engine's cached state don't inherit the panel.
void ResetHudViewport(ID3D11DeviceContext* ctx);

// Marks the right eye's render (from the Present hook) so GUI-pass detection uses that render's stack layout.
void SetSecondRender(bool on);

// While on, draws are the mod's own (vr/opaque_pass.cpp): passed straight through, never counted or hidden.
void SetModDraws(bool on);

} // namespace avp
