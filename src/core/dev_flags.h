#pragma once
// Developer switches for the investigation phase. These are compile-time on purpose: user-facing settings belong in
// the SteamVR overlay (project rule: no ini-only settings), which doesn't exist yet. When the overlay lands, anything
// here that a player could want moves there; the rest stays as developer-only build flags.

#include <cstdint>

namespace avp::dev {

// Trace hook groups (see hooks/trace_hooks.cpp for what each contains).
enum TraceGroup : uint32_t
{
    kTraceCamera    = 1u << 0,  // camera setters + drivers (per frame, cheap)
    kTraceRender    = 1u << 1,  // frame begin/end/present, device creation, viewport setup
    kTraceViews     = 1u << 2,  // per-view loops that run a few times per frame
    kTraceViewsHot  = 1u << 3,  // per-mesh / per-draw-packet submission (thousands per frame)
    kTraceLook      = 1u << 4,  // look controller per-tick + SetLookDirection
    kTraceLookHot   = 1u << 5,  // LookCtrl_GetAction (many per tick)
    kTraceInput     = 1u << 6,  // XInput poll/rumble, action packing
    kTraceInputHot  = 1u << 7,  // Input_GetActionValue (many per tick)
    kTraceCharacter = 1u << 8,  // skin attachment, component creation
    kTraceConsole   = 1u << 9,  // console tick/show/close
};

// Everything on for the first log-only runs: the hot groups' call counts are part of what we want to learn
// (e.g. per-mesh submits doubling once two views render). Turn the *Hot groups off if the frame rate suffers.
constexpr uint32_t kTraceGroups = 0xFFFFFFFFu;

// Seconds between per-hook call-count summaries in avpvr_trace.log.
constexpr unsigned kTraceSummaryIntervalMs = 5000;

// Milliseconds between detailed (argument-dump) lines for the same hook, after its first few calls.
constexpr unsigned kTraceDetailIntervalMs = 2000;

// Milliseconds between game-state snapshots in avpvr_snapshot.log (cameras, views, flags, D3D globals).
constexpr unsigned kSnapshotIntervalMs = 1000;

// EXPERIMENT B3 (docs/re/TODO_AND_DECISIONS.md): run the engine with two views, side by side, view 1 mirroring
// view 0's camera. OFF for the first log-only run so that run changes nothing in the game.
constexpr bool kTwoViewExperiment = false;

} // namespace avp::dev
