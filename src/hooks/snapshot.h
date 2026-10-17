#pragma once
// Background thread that writes a compact game-state snapshot every dev::kSnapshotIntervalMs to avpvr_snapshot.log:
// view count/index, each active view camera (position, angles, FOV, aspect, viewport rect), cinematic flag,
// console gate bytes, D3D globals and debug cvars. Pure reads, no hooks -- complements the call-level trace.

namespace avp {

void StartSnapshotThread();

} // namespace avp
