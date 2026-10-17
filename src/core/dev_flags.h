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

    // Slot hooks: write the stub into a vtable/IAT slot inside the exe image. ALSO trips CEG (2026-10-16: five
    // .rdata vtable pointers alone -> silent exit at the first level load). Kept only for slots outside the exe.
    kTraceCameraVT  = 1u << 16, // CamComp_ApplyToRenderCamera through the five camera vtables (CEG-UNSAFE)

    // Shadow entries: stubs placed in per-object shadow vtables on the heap (hooks/vmt_shadow.h). Nothing in the
    // exe image is written -- the CEG-safe way to hook virtual functions.
    kTraceCameraShadow = 1u << 24, // CamComp_ApplyToRenderCamera / CamComp_UpdatePosition on camera-component objects
};

// Steam CEG checksums the exe's read-only image (.text AND .rdata) during level loads and exits silently on any
// change (ExitProcess(0) from a wiped stack; kill stubs at 0083A1C0.., checkers 0083B0E0/0083B140). So the inline
// groups (bits 0-9, MinHook on game code) and slot groups (bits 16-23, on exe vtables/IAT) can only be used for
// menu-only investigation. docs/re/FINDINGS.md section 10.
constexpr uint32_t kInlineTraceGroups = 0x3FFu;
constexpr uint32_t kSlotTraceGroups = 0xFF0000u;

// Enabled groups: only CEG-safe ones.
constexpr uint32_t kTraceGroups = kTraceCameraShadow;

// Seconds between heap scans for objects to give shadow vtables (hooks/vmt_shadow.cpp). The first scans run until
// objects are found; a level load creates new objects, so scanning continues at this interval.
constexpr unsigned kShadowScanIntervalMs = 3000;

// Seconds between per-hook call-count summaries in avpvr_trace.log.
constexpr unsigned kTraceSummaryIntervalMs = 5000;

// Milliseconds between detailed (argument-dump) lines for the same hook, after its first few calls.
constexpr unsigned kTraceDetailIntervalMs = 2000;

// Milliseconds between game-state snapshots in avpvr_snapshot.log (cameras, views, flags, D3D globals).
constexpr unsigned kSnapshotIntervalMs = 1000;

// EXPERIMENT B3 (docs/re/TODO_AND_DECISIONS.md): run the engine with two views, side by side, view 1 mirroring
// view 0's camera. OFF: done 2026-10-16 -- the D3D11 renderer draws geometry once regardless of the view count
// (draw counts don't double); a second view only re-runs the dynamic-light pass for view 1, which leaves view 0 unlit.
// Native multi-view stereo is ruled out. Code kept (driven by the shadow camera hook, toggles 1/2 every 10 s).
constexpr bool kTwoViewExperiment = false;

// EXPERIMENT: render the 3D frame a second time per frame from the Present hook, camera yawed 30 degrees; the screen
// shows the normal frame's left half and the second render's right half (hooks/render_twice_experiment.h).
constexpr bool kRenderTwiceExperiment = true;

} // namespace avp::dev
