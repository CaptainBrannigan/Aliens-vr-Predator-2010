#include "two_view_experiment.h"

#include "trace_hooks.h"
#include "../core/addresses.h"
#include "../core/log.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <thread>

namespace avp {
namespace {

constexpr const char* kLog = "avpvr_twoview.log";

std::atomic<uint32_t> g_syncCount{0};

// Runs right before the player camera component writes the render camera (shadow hook on
// CamComp_ApplyToRenderCamera, main thread, once per frame). View 1 gets a full copy of view 0 as it was left by the
// previous frame (position, rotation, frustum, FOV, clip), then its own active flag and viewport. The current view is
// pointed back at view 0 first: the per-view loops switch it and don't restore it, and the camera setters write
// through it -- without this they could write this frame's camera into view 1.
void SyncViews(const TraceCtx*)
{
    ViewCamera* views = ViewCameras();
    const int previousIndex = Global<int>(addr::g_CurViewIndex);
    Global<uintptr_t>(addr::g_CurViewCamera) = reinterpret_cast<uintptr_t>(&views[0]);
    Global<int>(addr::g_CurViewIndex) = 0;
    Global<uint8_t>(addr::g_CurViewMask) = 1;

    std::memcpy(&views[1], &views[0], sizeof(ViewCamera));
    // Viewport rects stay full-screen: the D3D11 3D renderer ignores them (only 2D/HUD drawing reads +0x3C), and a
    // split only squeezes the HUD. The question now is whether view 1 is drawn at all (avpvr_d3d.log).
    for (int v = 0; v < 2; ++v)
    {
        views[v].viewport[0] = 1.0f; views[v].viewport[1] = 1.0f; views[v].viewport[2] = 0.0f; views[v].viewport[3] = 0.0f;
    }
    views[0].active = 1;
    views[1].active = 1;
    Global<uint8_t>(addr::g_CameraDirty) = 1;

    const uint32_t n = g_syncCount.fetch_add(1) + 1;
    if (n <= 3 || n % 600 == 0)
        AVP_LOG(kLog, "sync #%u: view0 pos=(%.2f %.2f %.2f) copied to view1, current view was %d, view count %d", n,
                views[0].pos[0], views[0].pos[1], views[0].pos[2], previousIndex, Global<int>(addr::g_ViewCount));
}

// Alternates the view count between 2 and 1 every 10 s once the per-frame sync is running, so one session measures
// both (avpvr_d3d.log prints the view count with each second's draw counts). Starts at 2 -- per-view arrays sized from
// the count at object creation stay large enough when it drops to 1.
void ToggleLoop()
{
    while (g_syncCount.load() == 0)
        Sleep(500);
    for (bool two = false;; two = !two)
    {
        Sleep(10000);
        Global<int>(addr::g_ViewCount) = two ? 2 : 1;
        AVP_LOG(kLog, "view count -> %d", two ? 2 : 1);
    }
}

} // namespace

void StartTwoViewExperiment()
{
    int& viewCount = Global<int>(addr::g_ViewCount);
    AVP_LOG(kLog, "two-view experiment ON: g_ViewCount %d -> 2", viewCount);
    viewCount = 2;

    // Inline hooks are off (Steam CEG); the shadow hook on the player camera component is the per-frame entry point.
    // It only exists once the scanner has swapped the camera object (a few seconds into a level).
    const bool attached = SetTracePre(addr::CamComp_ApplyToRenderCamera, &SyncViews);
    AVP_LOG(kLog, "per-frame sync attached to SH CamComp_ApplyToRenderCamera: %s", attached ? "yes" : "NO");
    std::thread(ToggleLoop).detach();
}

} // namespace avp
