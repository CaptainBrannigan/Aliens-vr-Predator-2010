#pragma once
// Movies (2026-10-18): the game plays them with Bink (binkw32.dll, imported). Some play during gameplay without a
// pause or the cinematic flag -- the Alien's, after an X press -- so the mod kept rendering them as VR frames, the
// movie squeezed into the HUD panel and partly clipped. BinkOpen / BinkDoFrame are hooked inside binkw32.dll (not the
// exe: CEG-safe); a large video decoding a frame lately means a movie is on (vr/stereo.cpp: flat screen). Small ones
// (animated textures on in-world screens) don't count.
//
// Log: avpvr_main.log (each video opened: file, size).

namespace avp::video {

// Once at startup (after MH_Initialize).
void Install();

// A movie (a video at least kMinMovieWidth wide) decoded a frame in the last quarter second. Any thread.
bool MoviePlaying();

} // namespace avp::video
