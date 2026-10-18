#include "openvr_session.h"

#include "dashboard.h"
#include "vr_input.h"
#include "../core/log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <openvr.h>

#include <cmath>
#include <cstring>

namespace avp::vr {
namespace {

constexpr const char* kLog = "avpvr_vr.log";
constexpr float kScreenDistanceMeters = 2.6f;
constexpr float kScreenWidthMeters = 3.2f;

using PFN_VR_InitInternal2 = uint32_t(__cdecl*)(::vr::EVRInitError* err, ::vr::EVRApplicationType type, const char* startup);
using PFN_VR_GetGenericInterface = void*(__cdecl*)(const char* version, ::vr::EVRInitError* err);

::vr::IVRSystem* g_system = nullptr;
::vr::IVRCompositor* g_compositor = nullptr;
::vr::IVROverlay* g_overlay = nullptr;
::vr::ETrackingUniverseOrigin g_universe = ::vr::TrackingUniverseStanding;
::vr::VROverlayHandle_t g_screen = ::vr::k_ulOverlayHandleInvalid;
bool g_screenVisible = false;
EyeGeometry g_eyes[2]{};
DWORD g_lastAttempt = 0;
bool g_failedPermanently = false;
HMODULE g_dll = nullptr;

bool IsVrServerRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32 e{};
    e.dwSize = sizeof(e);
    bool found = false;
    for (BOOL ok = Process32First(snap, &e); ok && !found; ok = Process32Next(snap, &e))
        found = _stricmp(e.szExeFile, "vrserver.exe") == 0;
    CloseHandle(snap);
    return found;
}

void Connect()
{
    HMODULE& dll = g_dll;
    if (!dll)
    {
        const std::string path = ModuleDirectory() + "openvr_api.dll";
        dll = LoadLibraryA(path.c_str());
        if (!dll)
        {
            AVP_LOG(kLog, "LoadLibrary(%s) failed (%lu) -- VR disabled", path.c_str(), GetLastError());
            g_failedPermanently = true;
            return;
        }
        AVP_LOG(kLog, "loaded %s", path.c_str());
    }
    auto init = reinterpret_cast<PFN_VR_InitInternal2>(GetProcAddress(dll, "VR_InitInternal2"));
    auto get = reinterpret_cast<PFN_VR_GetGenericInterface>(GetProcAddress(dll, "VR_GetGenericInterface"));
    if (!init || !get)
    {
        AVP_LOG(kLog, "openvr_api.dll exports missing -- VR disabled");
        g_failedPermanently = true;
        return;
    }

    ::vr::EVRInitError err = ::vr::VRInitError_None;
    init(&err, ::vr::VRApplication_Scene, nullptr);
    if (err != ::vr::VRInitError_None)
    {
        AVP_LOG(kLog, "VR_InitInternal2 error %d -- will retry", err);
        return;
    }
    g_system = static_cast<::vr::IVRSystem*>(get(::vr::IVRSystem_Version, &err));
    g_compositor = static_cast<::vr::IVRCompositor*>(get(::vr::IVRCompositor_Version, &err));
    g_overlay = static_cast<::vr::IVROverlay*>(get(::vr::IVROverlay_Version, &err));
    if (!g_system || !g_compositor)
    {
        AVP_LOG(kLog, "IVRSystem %p / IVRCompositor %p missing (err %d) -- VR disabled", g_system, g_compositor, err);
        g_system = nullptr;
        g_compositor = nullptr;
        g_failedPermanently = true;
        return;
    }
    g_universe = g_compositor->GetTrackingSpace();

    uint32_t w = 0, h = 0;
    g_system->GetRecommendedRenderTargetSize(&w, &h);
    for (int e = 0; e < 2; ++e)
    {
        EyeGeometry& g = g_eyes[e];
        g_system->GetProjectionRaw(static_cast<::vr::EVREye>(e), &g.tanLeft, &g.tanRight, &g.tanTop, &g.tanBottom);
        const ::vr::HmdMatrix34_t t = g_system->GetEyeToHeadTransform(static_cast<::vr::EVREye>(e));
        for (int i = 0; i < 3; ++i)
            g.offset[i] = t.m[i][3];
        AVP_LOG(kLog, "eye %d: tan L/R/T/B = %.3f/%.3f/%.3f/%.3f, eye-to-head offset (%.4f %.4f %.4f) m", e, g.tanLeft,
                g.tanRight, g.tanTop, g.tanBottom, g.offset[0], g.offset[1], g.offset[2]);
    }
    AVP_LOG(kLog, "CONNECTED: recommended %ux%u per eye, tracking universe %d, overlay %s", w, h, g_universe,
            g_overlay ? "yes" : "NO");
    dashboard::Start(g_overlay);
    auto* vrInput = static_cast<::vr::IVRInput*>(get(::vr::IVRInput_Version, &err));
    if (vrInput)
        input::Init(vrInput, g_system, static_cast<int>(g_universe));
    else
        AVP_LOG(kLog, "IVRInput missing (err %d) -- no controllers", err);
}

::vr::Texture_t DxTexture(ID3D11Texture2D* t)
{
    return ::vr::Texture_t{t, ::vr::TextureType_DirectX, ::vr::ColorSpace_Auto};
}

} // namespace

void TryConnect()
{
    if (g_compositor || g_failedPermanently)
        return;
    const DWORD now = GetTickCount();
    if (g_lastAttempt && now - g_lastAttempt < 2000)
        return;
    g_lastAttempt = now;
    if (!IsVrServerRunning())
        return;
    Connect();
}

bool IsConnected() { return g_compositor != nullptr; }

void ShutdownForExit(const char* why)
{
    static bool done = false;
    if (done)
        return;
    done = true;
    g_failedPermanently = true; // no reconnect from the frames still to come
    if (!g_compositor)
        return;
    AVP_LOG(kLog, "game quitting (%s): shutting SteamVR down", why);
    dashboard::Stop();
    AVP_LOG(kLog, "  dashboard stopped");
    input::Shutdown();
    {
        std::lock_guard<std::mutex> lock(OverlayMutex());
        if (g_overlay && g_screen != ::vr::k_ulOverlayHandleInvalid)
            g_overlay->HideOverlay(g_screen);
        g_screenVisible = false;
    }
    AVP_LOG(kLog, "  overlays hidden");
    g_compositor->ClearLastSubmittedFrame();
    AVP_LOG(kLog, "  last frame cleared");
    using PFN_VR_ShutdownInternal = void(__cdecl*)();
    auto shutdown = g_dll ? reinterpret_cast<PFN_VR_ShutdownInternal>(GetProcAddress(g_dll, "VR_ShutdownInternal")) : nullptr;
    g_compositor = nullptr;
    g_system = nullptr;
    g_overlay = nullptr;
    if (shutdown)
        shutdown();
    AVP_LOG(kLog, "SteamVR shut down%s", shutdown ? "" : " (no VR_ShutdownInternal export -- just disconnected)");
}

std::mutex& OverlayMutex()
{
    static auto* m = new std::mutex(); // leaked on purpose: no exit-time destructor
    return *m;
}

const EyeGeometry& Eye(int eye) { return g_eyes[eye & 1]; }

Pose WaitPoses()
{
    Pose p{};
    if (!g_compositor)
        return p;
    ::vr::TrackedDevicePose_t poses[::vr::k_unMaxTrackedDeviceCount]{};
    const ::vr::EVRCompositorError err = g_compositor->WaitGetPoses(poses, ::vr::k_unMaxTrackedDeviceCount, nullptr, 0);
    input::Update(); // controller actions for this frame (the virtual pad)
    const ::vr::TrackedDevicePose_t& hmd = poses[::vr::k_unTrackedDeviceIndex_Hmd];
    if (err == ::vr::VRCompositorError_None && hmd.bPoseIsValid)
    {
        std::memcpy(p.m, hmd.mDeviceToAbsoluteTracking.m, sizeof(p.m));
        p.valid = true;
    }
    return p;
}

void SubmitEyes(ID3D11Texture2D* left, const float boundsLeft[4], ID3D11Texture2D* right, const float boundsRight[4])
{
    if (!g_compositor)
        return;
    const ::vr::Texture_t l = DxTexture(left), r = DxTexture(right);
    const ::vr::VRTextureBounds_t bl{boundsLeft[0], boundsLeft[1], boundsLeft[2], boundsLeft[3]};
    const ::vr::VRTextureBounds_t br{boundsRight[0], boundsRight[1], boundsRight[2], boundsRight[3]};
    const ::vr::EVRCompositorError el = g_compositor->Submit(::vr::Eye_Left, &l, &bl);
    const ::vr::EVRCompositorError er = g_compositor->Submit(::vr::Eye_Right, &r, &br);
    static int logged = 0;
    if ((el || er) && logged++ < 10)
        AVP_LOG(kLog, "Submit errors: left %d, right %d", el, er);
}

void ShowScreen(ID3D11Texture2D* texture, float texelAspect, const Pose& head)
{
    if (!g_overlay)
        return;
    std::lock_guard<std::mutex> lock(OverlayMutex());
    if (g_screen == ::vr::k_ulOverlayHandleInvalid)
    {
        const ::vr::EVROverlayError e = g_overlay->CreateOverlay("avpvr.screen", "AvP VR screen", &g_screen);
        AVP_LOG(kLog, "CreateOverlay(screen): %d", e);
        if (e != ::vr::VROverlayError_None)
        {
            g_overlay = nullptr;
            return;
        }
        g_overlay->SetOverlayWidthInMeters(g_screen, kScreenWidthMeters);
    }
    if (!g_screenVisible && head.valid)
    {
        // Place it in front of the head, facing it, level (yaw only).
        const float fx = -head.m[0][2], fz = -head.m[2][2]; // forward = -Z column
        const float len = std::sqrt(fx * fx + fz * fz);
        const float dx = len > 1e-3f ? fx / len : 0.0f, dz = len > 1e-3f ? fz / len : -1.0f;
        // Overlay faces +Z; to face the viewer its +Z must point back at the head: z axis = -forward.
        ::vr::HmdMatrix34_t t{};
        t.m[0][0] = -dz; t.m[0][1] = 0; t.m[0][2] = -dx; t.m[0][3] = head.m[0][3] + dx * kScreenDistanceMeters;
        t.m[1][0] = 0;   t.m[1][1] = 1; t.m[1][2] = 0;   t.m[1][3] = head.m[1][3];
        t.m[2][0] = dx;  t.m[2][1] = 0; t.m[2][2] = -dz; t.m[2][3] = head.m[2][3] + dz * kScreenDistanceMeters;
        g_overlay->SetOverlayTransformAbsolute(g_screen, g_universe, &t);
        g_overlay->ShowOverlay(g_screen);
        g_screenVisible = true;
        AVP_LOG(kLog, "screen shown %.1f m ahead", kScreenDistanceMeters);
    }
    const ::vr::Texture_t tex = DxTexture(texture);
    g_overlay->SetOverlayTexture(g_screen, &tex);
    g_overlay->SetOverlayTexelAspect(g_screen, texelAspect);
}

void HideScreen()
{
    if (g_overlay && g_screenVisible)
    {
        std::lock_guard<std::mutex> lock(OverlayMutex());
        g_overlay->HideOverlay(g_screen);
        g_screenVisible = false;
        AVP_LOG(kLog, "screen hidden");
    }
}

} // namespace avp::vr
