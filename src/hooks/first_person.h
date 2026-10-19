#pragma once
// The first-person body and weapon models: identification, bone map, bone hide, and placing them (render only).
// Groundwork for motion controls (2026-10-17); replaces the log-only mesh probe.
//
// The mesh-queue pass (0053AB20) draws the player's body and gun with one skinned vertex shader whose bone buffer is
// VS constant buffer 5 (6288 bytes: world 4x4, world*view*projection 4x4, a float4, then 128 bones as transposed 3x4;
// docs/re/FINDINGS.md section 9). The game Maps / fills / Unmaps it once per model, then draws that model's parts.
//   - Each upload is snapshotted (Map / Unmap / UpdateSubresource hooks on the game's context).
//   - At a model's first draw it is classified: first person when its world translation is within 1.5 m of the game
//     camera; the first-person model with the most parts per frame is the BODY (arms, hands, ...), any other the
//     WEAPON.
//   - The snapshot can then be rewritten and uploaded again before the draw: body bones collapsed to a point (bone
//     hide: settings HideBodyBonesFirst..Last), the Marine's arms on the controllers (IK), the weapon's whole model
//     moved onto the right controller (GunOnRightController -- its pose relative to the game camera carried over to
//     the controller, plus offsets; the shots follow the same move, vr/shots.h).
//   - Bone map, once per body model: the body's vertex / index buffers are read back (staging copies), the blend
//     index / weight fields detected, and every bone's parts, weight and centroid (bind space) logged -- which bones
//     are the left arm, the right arm, the hands.
// Draw signatures (debug MeshProbe) as before. Log: avpvr_fp.log.

struct ID3D11DeviceContext;

namespace avp::fp {

// Once, with the game's immediate context (from the D3D probe's install thread).
void Install(ID3D11DeviceContext* ctx);

// From the draw hooks while an eye renders, before the real draw: pass = kHudPasses index (or -1), drawNo = 1-based
// within the pass; indexed draws pass their start index and base vertex.
void OnDraw(ID3D11DeviceContext* ctx, int pass, unsigned drawNo, unsigned indexCount, unsigned startIndex,
            int baseVertex, bool secondRender);

// From the Present hook (main thread): end of frame bookkeeping, the bone map when due.
void OnPresent(ID3D11DeviceContext* ctx);

// The Marine's weapon placement as a world -> world move (row vectors, v' = v * m): from the game's weapon model (as
// last drawn) onto the right controller with the gun offsets, from the controller's current pose -- the same move the
// drawn weapon gets (GunOnRightController). For shots from the gun (vr/shots.h). False when no first-person weapon was
// drawn in the last half second or there's no controller pose. Same thread as the game's update (it renders there).
// placedOrigin (optional) = the placed weapon model's origin (the controller + the gun offsets; at the hand).
// The weapon = a first-person model whose origin is at one of the game's own hands (props passing close by aren't).
bool GunPlacement(float m[16], float placedOrigin[3] = nullptr);

// The Marine's left palm is in the two-handed grab zone on the gun (or holds it): the left grip is the grab there, so
// the XInput hook drops the left bumper it also presses. Any thread.
bool LeftGripOnGun();

} // namespace avp::fp
