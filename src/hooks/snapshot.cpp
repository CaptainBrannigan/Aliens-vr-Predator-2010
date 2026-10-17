#include "snapshot.h"

#include "../core/addresses.h"
#include "../core/dev_flags.h"
#include "../core/log.h"

#include <windows.h>

#include <cstring>
#include <thread>

namespace avp {
namespace {

constexpr const char* kLog = "avpvr_snapshot.log";
constexpr float kRadToDeg = 57.2957795f;

bool SafeRead(void* dst, uintptr_t src, size_t bytes)
{
    __try
    {
        memcpy(dst, reinterpret_cast<const void*>(src), bytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename T>
T Read(uintptr_t address)
{
    T value{};
    SafeRead(&value, address, sizeof(T));
    return value;
}

void LogView(int index, const ViewCamera& v)
{
    AVP_LOG(kLog,
            "  view%d active=%u pos=(%.2f %.2f %.2f) pyr=(%.1f %.1f %.1f)deg fov h=%.2f v=%.2f deg aspect=%.4f "
            "viewport=(%.3f %.3f %.3f %.3f) clip=(%.3f %.1f)",
            index, v.active, v.pos[0], v.pos[1], v.pos[2], v.pitch * kRadToDeg, v.yaw * kRadToDeg,
            v.roll * kRadToDeg, 2 * v.hHalfFov * kRadToDeg, 2 * v.vHalfFov * kRadToDeg, v.aspect, v.viewport[0],
            v.viewport[1], v.viewport[2], v.viewport[3], v.clipE0, v.clipDC);
}

void SnapshotLoop()
{
    unsigned n = 0;
    for (;;)
    {
        Sleep(dev::kSnapshotIntervalMs);
        ++n;
        const int viewCount = Read<int>(addr::g_ViewCount);
        const int curView = Read<int>(addr::g_CurViewIndex);
        const uintptr_t curCam = Read<uintptr_t>(addr::g_CurViewCamera);
        AVP_LOG(kLog, "#%u views=%d current=%d (cam %08X) cinematic=%u onlyCurrent=%u flycam=%u fixcam=%u camUpdate=%u "
                      "renderEverything=%u",
                n, viewCount, curView, static_cast<unsigned>(curCam), Read<uint8_t>(addr::g_CinematicActive),
                Read<uint8_t>(addr::g_RenderOnlyCurrentView), Read<uint8_t>(addr::cvar_FlyCam),
                Read<uint8_t>(addr::cvar_FixCam), Read<uint8_t>(addr::cvar_CameraUpdate),
                Read<uint8_t>(addr::cvar_RenderEverything));

        for (int i = 0; i < kMaxViews; ++i)
        {
            ViewCamera v{};
            if (!SafeRead(&v, addr::g_ViewCameras + i * sizeof(ViewCamera), sizeof(v)))
                continue;
            if (i < 2 || v.active) // views 2-3 only when something switches them on
                LogView(i, v);
        }

        // Slow-changing state every 10th snapshot.
        if (n % 10 == 1)
        {
            AVP_LOG(kLog, "  d3d device=%08X ctx=%08X swapchain=%08X sceneTarget=%08X backbuffer=%08X bb=%dx%d "
                          "screen=%dx%d vsync=%u dt=%.4f",
                    Read<uint32_t>(addr::g_D3DDevice), Read<uint32_t>(addr::g_D3DContext),
                    Read<uint32_t>(addr::g_SwapChain), Read<uint32_t>(addr::g_SceneTarget),
                    Read<uint32_t>(addr::g_BackBuffer), Read<int>(addr::g_BackBufferWidth),
                    Read<int>(addr::g_BackBufferHeight), Read<int>(addr::g_ScreenWidth), Read<int>(addr::g_ScreenHeight),
                    Read<uint8_t>(addr::g_VSync), Read<float>(addr::g_FrameDt));
            AVP_LOG(kLog, "  console obj=%08X gateA(!=0)=%02X gateB(==0)=%02X gateC(==0)=%08X profile=%08X",
                    Read<uint32_t>(addr::g_Console), Read<uint8_t>(addr::g_ConsoleGateA),
                    Read<uint8_t>(addr::g_ConsoleGateB), Read<uint32_t>(addr::g_ConsoleGateC),
                    Read<uint32_t>(addr::g_PlayerProfile));
        }
    }
}

} // namespace

void StartSnapshotThread()
{
    std::thread(SnapshotLoop).detach();
}

} // namespace avp
