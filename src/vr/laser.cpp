#include "laser.h"

#include "openvr_session.h"
#include "../core/log.h"

#include <windows.h>
#include <openvr.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace avp::laser {
namespace {

constexpr const char* kLog = "avpvr_laser.log";
// OpenVR sizes an overlay by its width, the height following the texture's shape, so the strip's thickness is set
// by the texel aspect (texels kTexW x kTexH: aspect = kTexH * length / (kTexW * width)).
constexpr uint32_t kTexW = 1024;
constexpr uint32_t kTexH = 4;
constexpr float kWidthMeters = 0.003f;
constexpr DWORD kStaleMs = 250;

::vr::IVROverlay* g_overlay = nullptr;
::vr::ETrackingUniverseOrigin g_universe = ::vr::TrackingUniverseStanding;
::vr::VROverlayHandle_t g_handle = ::vr::k_ulOverlayHandleInvalid;

struct Beam
{
    bool visible = false;
    float m[12] = {}; // overlay transform, tracking space: columns = x (along the beam), y, z (toward the head)
    float length = 0.0f;
    DWORD tick = 0;
};
std::mutex g_mutex;
Beam g_beam; // guarded by g_mutex
HANDLE g_event = nullptr;
std::atomic<bool> g_stop{false};
std::thread g_thread;

void UploadTexture()
{
    std::vector<uint8_t> rgba(kTexW * kTexH * 4);
    const float rowAlpha[kTexH] = {0.35f, 1.0f, 1.0f, 0.35f};
    for (uint32_t y = 0; y < kTexH; ++y)
        for (uint32_t x = 0; x < kTexW; ++x)
        {
            uint8_t* p = &rgba[(y * kTexW + x) * 4];
            p[0] = 255;
            p[1] = 30;
            p[2] = 30;
            p[3] = static_cast<uint8_t>(rowAlpha[y] * 0.85f * 255.0f);
        }
    std::lock_guard<std::mutex> lock(vr::OverlayMutex());
    const ::vr::EVROverlayError e = g_overlay->SetOverlayRaw(g_handle, rgba.data(), kTexW, kTexH, 4);
    AVP_LOG(kLog, "beam texture uploaded: %d", e);
}

void ThreadProc()
{
    UploadTexture();
    bool shown = false;
    while (!g_stop.load())
    {
        WaitForSingleObject(g_event, 100);
        if (g_stop.load())
            break;
        Beam b;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            b = g_beam;
        }
        const bool visible = b.visible && GetTickCount() - b.tick < kStaleMs;
        std::lock_guard<std::mutex> lock(vr::OverlayMutex());
        if (visible)
        {
            ::vr::HmdMatrix34_t t;
            std::memcpy(&t.m[0][0], b.m, sizeof(b.m));
            g_overlay->SetOverlayWidthInMeters(g_handle, b.length);
            g_overlay->SetOverlayTexelAspect(g_handle, static_cast<float>(kTexH) * b.length /
                                                           (static_cast<float>(kTexW) * kWidthMeters));
            g_overlay->SetOverlayTransformAbsolute(g_handle, g_universe, &t);
            if (!shown)
            {
                g_overlay->ShowOverlay(g_handle);
                shown = true;
            }
        }
        else if (shown)
        {
            g_overlay->HideOverlay(g_handle);
            shown = false;
        }
    }
    if (shown)
    {
        std::lock_guard<std::mutex> lock(vr::OverlayMutex());
        g_overlay->HideOverlay(g_handle);
    }
}

} // namespace

void Start(::vr::IVROverlay* overlay, int universe)
{
    if (!overlay || g_thread.joinable())
        return;
    g_overlay = overlay;
    g_universe = static_cast<::vr::ETrackingUniverseOrigin>(universe);
    ::vr::EVROverlayError e;
    {
        std::lock_guard<std::mutex> lock(vr::OverlayMutex());
        e = g_overlay->CreateOverlay("avpvr.laser", "AvP VR laser", &g_handle);
    }
    AVP_LOG(kLog, "CreateOverlay(laser): %d", e);
    if (e != ::vr::VROverlayError_None)
        return;
    g_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_stop.store(false);
    g_thread = std::thread(ThreadProc);
}

void Stop()
{
    if (!g_thread.joinable())
        return;
    g_stop.store(true);
    SetEvent(g_event);
    g_thread.join();
}

void Update(bool visible, const float start[3], const float end[3], const float head[3])
{
    if (!g_thread.joinable())
        return;
    Beam b;
    b.tick = GetTickCount();
    if (visible)
    {
        // x along the beam, z toward the head (flat side to the viewer), y = z x x; the overlay's centre mid-beam.
        float x[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
        const float length = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        float mid[3], z[3];
        for (int k = 0; k < 3; ++k)
        {
            x[k] /= length > 1e-4f ? length : 1.0f;
            mid[k] = 0.5f * (start[k] + end[k]);
            z[k] = head[k] - mid[k];
        }
        const float along = z[0] * x[0] + z[1] * x[1] + z[2] * x[2];
        for (int k = 0; k < 3; ++k)
            z[k] -= along * x[k];
        const float zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
        if (length > 0.05f && zl > 1e-4f)
        {
            for (float& v : z)
                v /= zl;
            const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
            const float* axes[3] = {x, y, z};
            for (int col = 0; col < 3; ++col)
                for (int row = 0; row < 3; ++row)
                    b.m[row * 4 + col] = axes[col][row];
            for (int row = 0; row < 3; ++row)
                b.m[row * 4 + 3] = mid[row];
            b.length = length;
            b.visible = true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_beam = b;
    }
    SetEvent(g_event);
}

} // namespace avp::laser
