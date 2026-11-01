#pragma once
// Holsters / grab regions (ported from the MOHW mod's openvr_direct/vr_hands.cpp): spots on the body, in a frame that
// follows the head's position and, lazily, its yaw. A grip pressed with the palm in a spot presses that spot's pad
// button (settings: Holsters group), and the grip's own bumper stays silent for that press. A weapon spot (the
// Marine's D-pad selections) can also put the weapon in the hand that grabbed it: the same switch as the left-handed
// setting, so the arms' mirror, the gun and the triggers follow.
//
// Log: avpvr_holsters.log.

namespace avp::holsters {

// Once per frame, main thread (fp::OnPresent), after the two-handed aim decided.
void Update();

// The chest spot as a 3x4 pose (tracking space, as a controller's; facing the body's forward). False before the body
// frame has a heading.
bool ChestPose(float m[3][4]);

// The hand holding the weapon now: the left-handed setting, or the hand that last grabbed a weapon. Any thread.
bool LeftHandedNow();

// The pad's controller buttons through the holsters: the grips' bumpers held back, the spots' presses added. XInput hook.
unsigned short Apply(unsigned short buttons);

} // namespace avp::holsters
