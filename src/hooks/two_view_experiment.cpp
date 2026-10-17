#include "two_view_experiment.h"

#include "trace_hooks.h"
#include "../core/addresses.h"
#include "../core/log.h"

#include <windows.h>

#include <atomic>
#include <cstring>

namespace avp {
namespace {

constexpr const char* kLog = "avpvr_twoview.log";

std::atomic<uint32_t> g_syncCount{0};

// Runs before CamShake_ApplyAllViews and D3D_PresentFrame. View 1 gets a full copy of view 0 (position, rotation,
// frustum, FOV, clip), then its own active flag and viewport.
void SyncViews(const TraceCtx*)
{
    ViewCamera* views = ViewCameras();
    std::memcpy(&views[1], &views[0], sizeof(ViewCamera));
    views[0].viewport[0] = 0.5f; views[0].viewport[1] = 1.0f; views[0].viewport[2] = 0.0f; views[0].viewport[3] = 0.0f;
    views[1].viewport[0] = 0.5f; views[1].viewport[1] = 1.0f; views[1].viewport[2] = 0.5f; views[1].viewport[3] = 0.0f;
    views[0].active = 1;
    views[1].active = 1;
    Global<uint8_t>(addr::g_CameraDirty) = 1;

    const uint32_t n = g_syncCount.fetch_add(1) + 1;
    if (n <= 3 || n % 600 == 0)
        AVP_LOG(kLog, "sync #%u: view0 pos=(%.2f %.2f %.2f) copied to view1, viewports split, current view index %d", n,
                views[0].pos[0], views[0].pos[1], views[0].pos[2], Global<int>(addr::g_CurViewIndex));
}

} // namespace

void StartTwoViewExperiment()
{
    int& viewCount = Global<int>(addr::g_ViewCount);
    AVP_LOG(kLog, "two-view experiment ON: g_ViewCount %d -> 2", viewCount);
    viewCount = 2;

    const bool a = SetTracePre(addr::CamShake_ApplyAllViews, &SyncViews);
    const bool b = SetTracePre(addr::D3D_PresentFrame, &SyncViews);
    AVP_LOG(kLog, "per-frame sync attached: CamShake_ApplyAllViews=%s D3D_PresentFrame=%s", a ? "yes" : "NO",
            b ? "yes" : "NO");
}

} // namespace avp
