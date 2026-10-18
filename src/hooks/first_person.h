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
//     hide: settings HideBodyBonesFirst..Last), the weapon's whole model moved onto the right controller (test:
//     GunOnRightController -- its pose relative to the game camera carried over to the controller, plus offsets).
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

} // namespace avp::fp
