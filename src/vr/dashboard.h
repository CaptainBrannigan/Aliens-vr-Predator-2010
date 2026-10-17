#pragma once
// SteamVR dashboard tab ("AvP VR") listing every setting of core/settings.h's table, driven by SteamVR's laser
// pointer (ported from the MOHW mod's openvr_direct/vr_overlay.cpp). The menu is drawn with GDI into a memory bitmap
// and uploaded with SetOverlayRaw only when it changes, on its own thread, so it never touches the game's D3D device
// or stalls a frame. Overlay calls are serialized with vr::OverlayMutex().
//
// Log: avpvr_dashboard.log.

namespace vr {
class IVROverlay;
}

namespace avp::dashboard {

// Once, after the SteamVR connection is up. Creates the dashboard overlay and starts its thread.
void Start(::vr::IVROverlay* overlay);

} // namespace avp::dashboard
