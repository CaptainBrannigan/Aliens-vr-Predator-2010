#pragma once
// Makes a texture fully opaque: one full-screen triangle that writes only alpha = 1 (the colour is kept).
//
// The flat screen overlay shows the game's back buffer, whose alpha is whatever the passes left there: the menus write
// 1, the 3D scene and Bink movies 0. SteamVR showed the alpha-0 parts transparent -- black, the eyes behind the screen
// are black -- even with the overlay's premultiplied flag (2026-10-18: cutscenes black but for the Predator's cloak
// shimmer, an Alien cutscene's character black, movies black). The shaders are compiled at first use with
// d3dcompiler_47.dll (loaded dynamically, part of Windows). The game's pipeline state is saved and put back: the engine
// caches its own state and would not rebind it.

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace avp::opaque {

// texture needs D3D11_BIND_RENDER_TARGET. Render thread. False if the pass couldn't be set up (logged once).
bool MakeOpaque(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* texture);

} // namespace avp::opaque
