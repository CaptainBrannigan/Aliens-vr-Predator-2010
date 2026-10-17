#include "render_twice_experiment.h"

#include "trace_hooks.h"
#include "../core/addresses.h"
#include "../core/dev_flags.h"
#include "../core/log.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace avp {
namespace {

constexpr const char* kLog = "avpvr_render2.log";
constexpr float kYawDegrees = 30.0f;

using PFN_BeginFrame = void(__cdecl*)(char clear);   // FUN_00808DA0
using PFN_RenderScene = void(__cdecl*)();            // FUN_0080A440
using PFN_SetRotation = void(__cdecl*)(float* mat9); // Cam_SetRotationMatrix

const auto BeginFrame = reinterpret_cast<PFN_BeginFrame>(addr::Frame_BeginFrame);
const auto RenderScene = reinterpret_cast<PFN_RenderScene>(addr::Frame_RenderScene);
const auto SetRotation = reinterpret_cast<PFN_SetRotation>(addr::Cam_SetRotationMatrix);

// Set by the player camera component's per-frame update (shadow hook pre-callback), consumed at Present: the second
// render only runs on frames where the gameplay camera actually updated. First attempt gated on "camera position
// non-zero and no cutscene" and fired during the level load (the start camera is set before loading finishes) --
// running the in-game scene render there hung the main loop.
std::atomic<bool> g_cameraTicked{false};

void OnCameraApply(const TraceCtx*)
{
    g_cameraTicked.store(true, std::memory_order_relaxed);
}

ID3D11Texture2D* g_saved = nullptr; // copy of the normal frame
UINT g_width = 0, g_height = 0;
unsigned g_frames = 0;
bool g_failedOnce = false;

bool EnsureSavedTexture(ID3D11Device* device, ID3D11Resource* backBuffer)
{
    ID3D11Texture2D* bbTex = nullptr;
    if (FAILED(backBuffer->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bbTex))) || !bbTex)
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    bbTex->GetDesc(&desc);
    bbTex->Release();
    if (g_saved && desc.Width == g_width && desc.Height == g_height)
        return true;
    if (g_saved)
    {
        g_saved->Release();
        g_saved = nullptr;
    }
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &g_saved)))
        return false;
    g_width = desc.Width;
    g_height = desc.Height;
    AVP_LOG(kLog, "saved-frame texture %ux%u format %u", g_width, g_height, desc.Format);
    return true;
}

// Rotates each row of the 3x3 about world Y (Y is up: the player's Y stays constant while walking).
void YawRows(float* m, float degrees)
{
    const float r = degrees * 0.01745329252f, c = std::cos(r), s = std::sin(r);
    for (int row = 0; row < 3; ++row)
    {
        float* v = m + row * 3;
        const float x = v[0], z = v[2];
        v[0] = x * c + z * s;
        v[2] = -x * s + z * c;
    }
}

} // namespace

void StartRenderTwiceExperiment()
{
    if (!dev::kRenderTwiceExperiment)
        return;
    const bool attached = SetTracePre(addr::CamComp_ApplyToRenderCamera, &OnCameraApply);
    AVP_LOG(kLog, "gameplay gate attached to SH CamComp_ApplyToRenderCamera: %s", attached ? "yes" : "NO");
}

void RenderTwiceOnPresent(IDXGISwapChain*)
{
    if (!dev::kRenderTwiceExperiment)
        return;

    ViewCamera* views = ViewCameras();
    // Only on gameplay frames: the player camera updated since the last Present, and no cutscene.
    if (!g_cameraTicked.exchange(false, std::memory_order_relaxed) || Global<uint8_t>(addr::g_CinematicActive))
        return;

    auto* device = reinterpret_cast<ID3D11Device*>(Global<uintptr_t>(addr::g_D3DDevice));
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(Global<uintptr_t>(addr::g_D3DContext));
    auto* backBuffer = reinterpret_cast<ID3D11Resource*>(Global<uintptr_t>(addr::g_BackBuffer));
    auto* scene = reinterpret_cast<ID3D11Resource*>(Global<uintptr_t>(addr::g_SceneTarget));
    if (!device || !ctx || !backBuffer || !scene || !EnsureSavedTexture(device, backBuffer))
    {
        if (!g_failedOnce)
            AVP_LOG(kLog, "missing D3D objects (device %p ctx %p bb %p scene %p)", device, ctx, backBuffer, scene);
        g_failedOnce = true;
        return;
    }

    // 1. Keep the normal frame.
    ctx->CopyResource(g_saved, backBuffer);

    // 2. Second camera: view 0 yawed.
    ViewCamera saved;
    std::memcpy(&saved, &views[0], sizeof(ViewCamera));
    Global<uintptr_t>(addr::g_CurViewCamera) = reinterpret_cast<uintptr_t>(&views[0]);
    Global<int>(addr::g_CurViewIndex) = 0;
    Global<uint8_t>(addr::g_CurViewMask) = 1;
    float rot[9];
    std::memcpy(rot, saved.rot, sizeof(rot));
    YawRows(rot, kYawDegrees);
    SetRotation(rot);
    if (g_frames == 0)
        AVP_LOG(kLog, "first second render starting (frame counter %d, job kick frame %d, %d workers)",
                Global<int>(addr::g_FrameCounter), Global<int>(addr::g_JobBatchKickFrame),
                Global<int>(addr::g_JobBatchWorkerCount));

    // 3. Render the 3D frame again, exactly as the in-game state does (00812331..0081233D). The per-frame job batch
    // was already joined by the first render; make the second render skip that join (it would wait forever on
    // consumed events -- live hang 2026-10-16, main thread in FUN_004F6B90 -> WaitForSingleObject).
    int& kickFrame = Global<int>(addr::g_JobBatchKickFrame);
    const int savedKickFrame = kickFrame;
    if (kickFrame == Global<int>(addr::g_FrameCounter))
        kickFrame = savedKickFrame - 1;
    const DWORD t0 = GetTickCount();
    BeginFrame(1);
    RenderScene();
    const DWORD ms = GetTickCount() - t0;
    kickFrame = savedKickFrame;

    // 4. Normal frame back, then the second render's right half over it.
    ctx->CopyResource(backBuffer, g_saved);
    D3D11_BOX box{g_width / 2, 0, 0, g_width, g_height, 1};
    ctx->CopySubresourceRegion(backBuffer, 0, g_width / 2, 0, 0, scene, 0, &box);

    // 5. Camera back.
    std::memcpy(&views[0], &saved, sizeof(ViewCamera));
    Global<uint8_t>(addr::g_CameraDirty) = 1;

    ++g_frames;
    if (g_frames <= 3 || g_frames % 300 == 0)
        AVP_LOG(kLog, "frame %u: second render took %lu ms (CPU side), camera pos (%.2f %.2f %.2f), job join %s", g_frames,
                ms, saved.pos[0], saved.pos[1], saved.pos[2],
                savedKickFrame == Global<int>(addr::g_FrameCounter) ? "skipped" : "not pending");
}

} // namespace avp
