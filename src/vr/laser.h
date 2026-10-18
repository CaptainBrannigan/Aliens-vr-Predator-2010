#pragma once
// Laser along the shots (setting LaserSight): a thin red beam from the shot origin along the shot direction, a fixed
// length -- no hit test, so it shows through walls -- for lining up the shot origin and angle offsets with the drawn
// gun. A SteamVR overlay, as in the MOHW mod: a strip along the beam, turned about its own length to face the head,
// placed in tracking space by its own thread (IVROverlay calls under vr::OverlayMutex()) from the line the game thread
// hands it once per frame (vr/shots.cpp).
//
// Log: avpvr_laser.log.

namespace vr {
class IVROverlay;
}

namespace avp::laser {

// Once, after the SteamVR connection is up: creates the overlay and starts its thread. universe = the session's
// ETrackingUniverseOrigin.
void Start(::vr::IVROverlay* overlay, int universe);

// Stops the thread; no overlay calls after it returns. Before VR_Shutdown.
void Stop();

// Main thread, once per frame: the beam in tracking space (metres), or hidden. The thread hides it by itself when no
// update arrives for a quarter second (menus, cutscenes).
void Update(bool visible, const float start[3], const float end[3], const float head[3]);

} // namespace avp::laser
