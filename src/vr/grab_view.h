#pragma once
// The grab zone view (setting ShowGrabZones, ported from the MOHW mod): every place a grip does something, drawn where it
// really is, for setting them up. One SteamVR overlay per slot, tracking space: the holster spots as rings, the
// two-handed grab as a band along the barrel. Green = a hand is in it; grey = a spot with nothing assigned.
//
// Log: avpvr_grabview.log.

namespace vr {
class IVROverlay;
}

namespace avp::grabview {

constexpr int kZoneSlots = 7; // the holster spots, in holsters.cpp's kZones order
constexpr int kTwoHandSlot = 7;
constexpr int kSlotCount = 8;

struct Shape
{
    bool visible = false;
    bool billboard = false; // faces the head at pos; otherwise m: 3x4, columns = its axes, +Z its face
    bool active = false;    // a hand is in it
    bool assigned = true;   // a grip here does something
    float pos[3] = {};
    float m[12] = {};
    float width = 0.0f, height = 0.0f; // metres
};

void Start(::vr::IVROverlay* overlay, int universe);
void Stop();

// Main thread, every frame while the view is on; the overlays hide by themselves when this stops coming.
void Update(const Shape shapes[kSlotCount], const float head[3]);

} // namespace avp::grabview
