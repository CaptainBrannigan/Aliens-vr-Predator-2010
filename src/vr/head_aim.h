#pragma once
// Head aim: the head drives the player's look (aim), through the look controller's SetLookDirection (vtable +0x9C,
// replaced in the controllers' shadow vtables -- hooks/vmt_shadow.cpp). Replaces the log-only look probe.
//
// The game's look (docs/re/FINDINGS.md section 2) is rebuilt every frame in the character's BODY frame; yaw past
// ~90 deg from the body turns the body; the Alien's body frame follows walls and ceilings. So the mod keeps its own
// PLAYER FRAME P: vertical = the body's (from the game's body matrix), forward = our own heading, which only the right
// stick turns (smooth turn, TurnSpeedDeg) and which is carried along when that axis changes. It never follows the
// body's yaw, so the body turning under a far-sideways look feeds nothing back. Each look update:
//   look = forward of (P * head rotation), clamped to the controller's own pitch limits -> the original
//   SetLookDirection, which stores it, hands it to the character and raycasts the aim point exactly as for a stick.
// The eye cameras use the same P * head (vr/stereo.cpp), so view and aim agree and the Alien's view is upright on
// its surface.
//
// Multiplayer: the look is set through the game's own setter, so whatever the game replicates from its look state
// carries it; if the game turns out to send stick input instead, this needs feeding as input (see FINDINGS).
//
// Recenter keeps the view: the heading becomes the current look's. Off while the setting is off or outside VR
// gameplay (cutscenes, menus): the game's own look update runs untouched then.
//
// Log: avpvr_aim.log.

namespace avp::aim {

// Registers the shadow-vtable replacement. Call before StartVmtShadow().
void Register();

// The player frame of the latest look update (3x3 row-major, columns = right, up, forward in world space), while
// head aim is driving the look; look (optional) = the look's own frame without roll (forward = the look sent, up from
// the player frame), which the game's camera was built from. False otherwise (the eye camera then uses the game's yaw).
bool PlayerFrame(float m[9], float look[9] = nullptr);

} // namespace avp::aim
