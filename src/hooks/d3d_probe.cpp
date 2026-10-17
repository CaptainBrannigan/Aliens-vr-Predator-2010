#include "d3d_probe.h"

#include "render_twice_experiment.h"
#include "../vr/stereo.h"
#include "../core/addresses.h"
#include "../core/log.h"

#include <MinHook.h>
#include <windows.h>
#include <d3d11.h>
#include <intrin.h>

#include <atomic>
#include <thread>

namespace avp {
namespace {

constexpr const char* kLog = "avpvr_d3d.log";

// Vtable indices (d3d11.h / dxgi.h declaration order).
constexpr int kCtxDrawIndexed = 12;
constexpr int kCtxDraw = 13;
constexpr int kCtxDrawIndexedInstanced = 20;
constexpr int kCtxDrawInstanced = 21;
constexpr int kCtxRSSetViewports = 44;
constexpr int kSwapPresent = 8;

using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_RSSetViewports = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

PFN_DrawIndexed g_realDrawIndexed = nullptr;
PFN_Draw g_realDraw = nullptr;
PFN_DrawIndexedInstanced g_realDrawIndexedInstanced = nullptr;
PFN_DrawInstanced g_realDrawInstanced = nullptr;
PFN_RSSetViewports g_realRSSetViewports = nullptr;
PFN_Present g_realPresent = nullptr;

// Per-frame counters (render thread writes; Present snapshots them into the per-second accumulators).
struct Counters
{
    uint32_t draws = 0;
    uint32_t drawsView[3] = {}; // current view 0, 1, other
    uint32_t viewports = 0;
};
Counters g_frame;   // current frame
Counters g_second;  // accumulated over the current second
uint32_t g_framesThisSecond = 0;
DWORD g_secondStart = 0;
int g_viewCountSeen = 0; // view count at the last Present of the second (changes mid-second are noted)
bool g_viewCountChanged = false;

struct SeenViewport
{
    float x, y, w, h;
};
SeenViewport g_seen[32];
int g_seenCount = 0;

// ---- Who issues the draws ----
// Every 32nd draw, the stack is scanned for return addresses into the exe: the first one is the renderer function
// right above D3D (the "direct" caller), and the first one inside the main-loop range is the per-frame call that
// contains the rendering. Histograms are logged once per second, then reset.
constexpr uintptr_t kExeCodeBegin = 0x00401000, kExeCodeEnd = 0x008BC000;
constexpr uintptr_t kMainLoopBegin = 0x00808000, kMainLoopEnd = 0x00814000;
constexpr int kHistSlots = 24;
struct Hist
{
    uint32_t address[kHistSlots] = {};
    uint32_t count[kHistSlots] = {};
    void Add(uint32_t a)
    {
        for (int i = 0; i < kHistSlots; ++i)
        {
            if (address[i] == a || address[i] == 0)
            {
                address[i] = a;
                ++count[i];
                return;
            }
        }
    }
};
Hist g_directCallers, g_loopCallers;
uint32_t g_drawSample = 0;

bool SafeReadStack(uintptr_t a, uint32_t* out)
{
    __try
    {
        *out = *reinterpret_cast<const uint32_t*>(a);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void SampleDrawCallers(uintptr_t esp)
{
    bool haveDirect = false, haveLoop = false;
    for (int i = 0; i < 1024 && !(haveDirect && haveLoop); ++i)
    {
        uint32_t v = 0;
        if (!SafeReadStack(esp + i * 4, &v))
            break;
        if (v < kExeCodeBegin || v >= kExeCodeEnd)
            continue;
        if (!haveDirect)
        {
            g_directCallers.Add(v);
            haveDirect = true;
        }
        if (!haveLoop && v >= kMainLoopBegin && v < kMainLoopEnd)
        {
            g_loopCallers.Add(v);
            haveLoop = true;
        }
    }
}

void LogHist(const char* title, Hist& h)
{
    char line[1024];
    int used = _snprintf_s(line, sizeof(line), _TRUNCATE, "  %s:", title);
    // Highest counts first (selection, small table).
    bool taken[kHistSlots] = {};
    for (int n = 0; n < 10; ++n)
    {
        int best = -1;
        for (int i = 0; i < kHistSlots; ++i)
            if (h.address[i] && !taken[i] && (best < 0 || h.count[i] > h.count[best]))
                best = i;
        if (best < 0)
            break;
        taken[best] = true;
        if (used > 0 && used < static_cast<int>(sizeof(line)))
            used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, " %08X x%u", h.address[best],
                                h.count[best]);
    }
    AVP_LOG(kLog, "%s", line);
    h = Hist{};
}

inline void CountDraw()
{
    ++g_frame.draws;
    const int view = Global<int>(addr::g_CurViewIndex);
    ++g_frame.drawsView[view == 0 ? 0 : view == 1 ? 1 : 2];
    if ((++g_drawSample & 31) == 0)
        SampleDrawCallers(reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()));
}

void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base)
{
    CountDraw();
    g_realDrawIndexed(c, count, start, base);
}
void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* c, UINT count, UINT start)
{
    CountDraw();
    g_realDraw(c, count, start);
}
void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f)
{
    CountDraw();
    g_realDrawIndexedInstanced(c, a, b, d, e, f);
}
void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e)
{
    CountDraw();
    g_realDrawInstanced(c, a, b, d, e);
}

void STDMETHODCALLTYPE Hook_RSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vps)
{
    ++g_frame.viewports;
    if (n > 0 && vps && g_seenCount < 32)
    {
        const D3D11_VIEWPORT& v = vps[0];
        bool known = false;
        for (int i = 0; i < g_seenCount && !known; ++i)
            known = g_seen[i].x == v.TopLeftX && g_seen[i].y == v.TopLeftY && g_seen[i].w == v.Width &&
                    g_seen[i].h == v.Height;
        if (!known)
        {
            g_seen[g_seenCount++] = {v.TopLeftX, v.TopLeftY, v.Width, v.Height};
            AVP_LOG(kLog, "new viewport #%d: x=%.0f y=%.0f w=%.0f h=%.0f (count %u, current view %d)", g_seenCount,
                    v.TopLeftX, v.TopLeftY, v.Width, v.Height, n, Global<int>(addr::g_CurViewIndex));
        }
    }
    g_realRSSetViewports(c, n, vps);
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    g_second.draws += g_frame.draws;
    for (int i = 0; i < 3; ++i)
        g_second.drawsView[i] += g_frame.drawsView[i];
    g_second.viewports += g_frame.viewports;
    g_frame = Counters{};
    ++g_framesThisSecond;

    const int viewCount = Global<int>(addr::g_ViewCount);
    if (g_framesThisSecond > 1 && viewCount != g_viewCountSeen)
        g_viewCountChanged = true;
    g_viewCountSeen = viewCount;

    const DWORD now = GetTickCount();
    if (g_secondStart == 0)
        g_secondStart = now;
    if (now - g_secondStart >= 1000)
    {
        const float f = g_framesThisSecond ? static_cast<float>(g_framesThisSecond) : 1.0f;
        AVP_LOG(kLog,
                "views=%d%s fps=%u draws/frame=%.0f (view0 %.0f, view1 %.0f, other %.0f) viewports/frame=%.1f",
                g_viewCountSeen, g_viewCountChanged ? " (changed this second)" : "", g_framesThisSecond,
                g_second.draws / f, g_second.drawsView[0] / f, g_second.drawsView[1] / f, g_second.drawsView[2] / f,
                g_second.viewports / f);
        LogHist("draw callers (first exe return address, sampled 1/32)", g_directCallers);
        LogHist("main-loop frames containing the draws", g_loopCallers);
        g_second = Counters{};
        g_framesThisSecond = 0;
        g_viewCountChanged = false;
        g_secondStart = now;
    }
    RenderTwiceOnPresent(sc); // old experiment (off); draws it issues are counted in the next frame
    unsigned syncInterval = sync;
    stereo::OnPresent(sc, syncInterval); // VR: second eye render + submit, or the flat screen
    return g_realPresent(sc, syncInterval, flags);
}

template <typename T>
bool HookVtableEntry(void* object, int index, void* detour, T* original, const char* name)
{
    void** vtable = *reinterpret_cast<void***>(object);
    void* target = vtable[index];
    MH_STATUS s = MH_CreateHook(target, detour, reinterpret_cast<void**>(original));
    if (s == MH_OK)
        s = MH_EnableHook(target);
    AVP_LOG(kLog, "hook %-22s at %p: %s", name, target, MH_StatusToString(s));
    return s == MH_OK;
}

void ProbeThread()
{
    // The device is created at startup; poll until the game has stored its context and swapchain.
    for (int i = 0; i < 600; ++i)
    {
        auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(Global<uintptr_t>(addr::g_D3DContext));
        auto* swap = reinterpret_cast<IDXGISwapChain*>(Global<uintptr_t>(addr::g_SwapChain));
        if (ctx && swap)
        {
            AVP_LOG(kLog, "game context %p, swapchain %p -- installing hooks in d3d11/dxgi", ctx, swap);
            HookVtableEntry(ctx, kCtxDrawIndexed, reinterpret_cast<void*>(&Hook_DrawIndexed), &g_realDrawIndexed,
                            "DrawIndexed");
            HookVtableEntry(ctx, kCtxDraw, reinterpret_cast<void*>(&Hook_Draw), &g_realDraw, "Draw");
            HookVtableEntry(ctx, kCtxDrawIndexedInstanced, reinterpret_cast<void*>(&Hook_DrawIndexedInstanced),
                            &g_realDrawIndexedInstanced, "DrawIndexedInstanced");
            HookVtableEntry(ctx, kCtxDrawInstanced, reinterpret_cast<void*>(&Hook_DrawInstanced),
                            &g_realDrawInstanced, "DrawInstanced");
            HookVtableEntry(ctx, kCtxRSSetViewports, reinterpret_cast<void*>(&Hook_RSSetViewports),
                            &g_realRSSetViewports, "RSSetViewports");
            HookVtableEntry(swap, kSwapPresent, reinterpret_cast<void*>(&Hook_Present), &g_realPresent, "Present");
            return;
        }
        Sleep(500);
    }
    AVP_LOG(kLog, "gave up waiting for the game's D3D11 context/swapchain");
}

} // namespace

void StartD3DProbe()
{
    std::thread(ProbeThread).detach();
}

} // namespace avp
