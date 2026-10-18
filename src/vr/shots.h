#pragma once
// Shots from the gun: the Marine's shots leave from the weapon on the right controller and go along it, while the look
// (and the view) stays on the head (vr/head_aim.h).
//
// Every ranged weapon asks itself for a fire transform before a shot (GetFireTransform, weapon vtable +0xE4; static
// analysis in docs/re/FINDINGS.md section 13): the position (a muzzle attachment, near the game's hand) and the
// direction (the character's look). The shot spawn uses exactly those two. The mod replaces +0xE4 in the ranged
// weapons' shadow vtables (hooks/vmt_shadow.cpp): the original runs, then -- for the local player's character, as
// the Marine, with ShotsFromGun on -- the shot's line is put on the drawn gun: from the placed weapon model's origin
// plus the shot origin offsets (in the gun's frame), along the game's direction through the same move that puts the
// drawn weapon on the controller (hooks/first_person.h GunPlacement) plus the shot angle offsets. The laser
// (vr/laser.h) shows the same line. Other characters (AI, other players) are untouched.
//
// Log: avpvr_shots.log.

namespace avp::shots {

// Registers the shadow-vtable replacements. Call before StartVmtShadow().
void Register();

// From head aim's look update (main thread, after the game's own), once per frame: the laser along the shots' line,
// from the look the game was just given (the direction a shot would get).
void OnLook(void* character, const float look[3]);

} // namespace avp::shots
