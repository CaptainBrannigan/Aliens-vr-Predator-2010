#include "grab_view.h"

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

namespace avp::grabview {
namespace {

constexpr const char* kLog = "avpvr_grabview.log";
constexpr DWORD kStaleMs = 250;
constexpr int kTexSize = 256;

struct Slot
{
    const char* key;
    const char* label;
    bool band;
    float r, g, b;
};
constexpr Slot kSlots[kSlotCount] = {
    {"avpvr.zone.stomachl", "Stomach", false, 1.0f, 0.8f, 0.2f},
    {"avpvr.zone.stomachr", "Stomach", false, 1.0f, 0.8f, 0.2f},
    {"avpvr.zone.hipl", "Hip", false, 1.0f, 0.5f, 0.2f},
    {"avpvr.zone.hipr", "Hip", false, 1.0f, 0.5f, 0.2f},
    {"avpvr.zone.chest", "Chest", false, 0.3f, 0.7f, 1.0f},
    {"avpvr.zone.shoulderl", "L shoulder", false, 0.8f, 0.4f, 1.0f},
    {"avpvr.zone.shoulderr", "R shoulder", false, 0.8f, 0.4f, 1.0f},
    {"avpvr.zone.twohand", nullptr, true, 0.3f, 1.0f, 0.6f},
};
constexpr float kUnassigned = 0.45f;

::vr::IVROverlay* g_overlay = nullptr;
::vr::ETrackingUniverseOrigin g_universe = ::vr::TrackingUniverseStanding;
::vr::VROverlayHandle_t g_handles[kSlotCount] = {};

std::mutex g_mutex;
Shape g_shapes[kSlotCount]; // guarded by g_mutex
float g_head[3] = {};       // guarded by g_mutex
DWORD g_tick = 0;           // guarded by g_mutex
HANDLE g_event = nullptr;
std::atomic<bool> g_stop{false};
std::thread g_thread;

// The label, white, centred: GDI text on black, its coverage taken as alpha.
void DrawLabel(std::vector<uint8_t>& rgba, const char* text)
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kTexSize;
    bi.bmiHeader.biHeight = -kTexSize;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    uint8_t* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
    if (dc && bitmap && bits)
    {
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        HFONT font = CreateFontA(kTexSize / 7, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r = {0, 0, kTexSize, kTexSize};
        DrawTextA(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        GdiFlush();
        for (int i = 0; i < kTexSize * kTexSize; ++i)
            if (bits[i * 4 + 1] > rgba[i * 4 + 3])
                rgba[i * 4 + 3] = bits[i * 4 + 1];
        SelectObject(dc, oldFont);
        SelectObject(dc, oldBitmap);
        DeleteObject(font);
    }
    if (bitmap)
        DeleteObject(bitmap);
    if (dc)
        DeleteDC(dc);
}

void UploadTexture(int slot)
{
    const Slot& s = kSlots[slot];
    std::vector<uint8_t> rgba(kTexSize * kTexSize * 4, 255);
    for (int y = 0; y < kTexSize; ++y)
        for (int x = 0; x < kTexSize; ++x)
        {
            const float u = (x + 0.5f) / kTexSize * 2.0f - 1.0f, v = (y + 0.5f) / kTexSize * 2.0f - 1.0f;
            const float r = std::sqrt(u * u + v * v);
            const float alpha = s.band ? (std::fabs(v) > 0.92f ? 0.9f : 0.16f) : r > 1.0f ? 0.0f : r > 0.965f ? 1.0f : 0.07f;
            rgba[(y * kTexSize + x) * 4 + 3] = static_cast<uint8_t>(alpha * 255.0f);
        }
    if (s.label)
        DrawLabel(rgba, s.label);
    std::lock_guard<std::mutex> lock(vr::OverlayMutex());
    g_overlay->SetOverlayRaw(g_handles[slot], rgba.data(), kTexSize, kTexSize, 4);
}

// Facing the head at pos, upright; looking straight down at it, whatever's level.
bool Billboard(const float pos[3], const float head[3], float m[12])
{
    float z[3] = {head[0] - pos[0], head[1] - pos[1], head[2] - pos[2]};
    const float len = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (len < 1e-3f)
        return false;
    for (float& v : z)
        v /= len;
    float x[3] = {z[2], 0.0f, -z[0]}; // up x z
    float xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (xl < 0.05f)
    {
        x[0] = 1.0f, x[1] = 0.0f, x[2] = 0.0f;
        const float along = x[0] * z[0];
        for (int k = 0; k < 3; ++k)
            x[k] -= along * z[k];
        xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    }
    for (float& v : x)
        v /= xl;
    const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    const float* axes[3] = {x, y, z};
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            m[row * 4 + col] = axes[col][row];
    for (int row = 0; row < 3; ++row)
        m[row * 4 + 3] = pos[row];
    return true;
}

void ThreadProc()
{
    bool shown[kSlotCount] = {}, uploaded[kSlotCount] = {};
    int tint[kSlotCount];
    for (int& t : tint)
        t = -1;
    while (!g_stop.load())
    {
        WaitForSingleObject(g_event, 100);
        if (g_stop.load())
            break;
        Shape shapes[kSlotCount];
        float head[3];
        bool fresh;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            std::memcpy(shapes, g_shapes, sizeof(shapes));
            std::memcpy(head, g_head, sizeof(head));
            fresh = GetTickCount() - g_tick < kStaleMs;
        }
        for (int i = 0; i < kSlotCount; ++i)
        {
            if (!g_handles[i])
                continue;
            const Shape& s = shapes[i];
            ::vr::HmdMatrix34_t t;
            bool placed = fresh && s.visible && s.width > 0.0f && s.height > 0.0f;
            if (placed && s.billboard)
                placed = Billboard(s.pos, head, &t.m[0][0]);
            else if (placed)
                std::memcpy(&t.m[0][0], s.m, sizeof(s.m));
            if (!placed)
            {
                if (shown[i])
                {
                    std::lock_guard<std::mutex> lock(vr::OverlayMutex());
                    g_overlay->HideOverlay(g_handles[i]);
                    shown[i] = false;
                }
                continue;
            }
            if (!uploaded[i])
            {
                UploadTexture(i);
                uploaded[i] = true;
            }
            std::lock_guard<std::mutex> lock(vr::OverlayMutex());
            const int wanted = s.active ? 1 : s.assigned ? 0 : 2;
            if (tint[i] != wanted)
            {
                const Slot& d = kSlots[i];
                if (wanted == 1)
                    g_overlay->SetOverlayColor(g_handles[i], 0.15f, 1.0f, 0.25f);
                else if (wanted == 2)
                    g_overlay->SetOverlayColor(g_handles[i], kUnassigned, kUnassigned, kUnassigned);
                else
                    g_overlay->SetOverlayColor(g_handles[i], d.r, d.g, d.b);
                tint[i] = wanted;
            }
            g_overlay->SetOverlayWidthInMeters(g_handles[i], s.width);
            g_overlay->SetOverlayTexelAspect(g_handles[i], s.width / s.height);
            g_overlay->SetOverlayTransformAbsolute(g_handles[i], g_universe, &t);
            if (!shown[i])
            {
                g_overlay->ShowOverlay(g_handles[i]);
                shown[i] = true;
            }
        }
    }
    std::lock_guard<std::mutex> lock(vr::OverlayMutex());
    for (int i = 0; i < kSlotCount; ++i)
        if (shown[i])
            g_overlay->HideOverlay(g_handles[i]);
}

} // namespace

void Start(::vr::IVROverlay* overlay, int universe)
{
    if (!overlay || g_thread.joinable())
        return;
    g_overlay = overlay;
    g_universe = static_cast<::vr::ETrackingUniverseOrigin>(universe);
    int created = 0;
    {
        std::lock_guard<std::mutex> lock(vr::OverlayMutex());
        for (int i = 0; i < kSlotCount; ++i)
        {
            if (g_overlay->CreateOverlay(kSlots[i].key, kSlots[i].key, &g_handles[i]) == ::vr::VROverlayError_None)
                ++created;
            else
                g_handles[i] = 0;
        }
    }
    AVP_LOG(kLog, "%d of %d overlays created", created, kSlotCount);
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

void Update(const Shape shapes[kSlotCount], const float head[3])
{
    if (!g_thread.joinable())
        return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::memcpy(g_shapes, shapes, sizeof(g_shapes));
        std::memcpy(g_head, head, sizeof(g_head));
        g_tick = GetTickCount();
    }
    SetEvent(g_event);
}

} // namespace avp::grabview
