#include "d3d_probe.h"

#include "render_twice_experiment.h"
#include "hud_passes.h"
#include "quit_hook.h"
#include "../vr/stereo.h"
#include "../core/settings.h"
#include "../core/addresses.h"
#include "../core/log.h"

#include <MinHook.h>
#include <windows.h>
#include <d3d11.h>
#include <intrin.h>

#include <atomic>
#include <cmath>
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
constexpr int kCtxRSSetScissorRects = 45;
constexpr int kCtxOMSetRenderTargets = 33;
constexpr int kCtxDrawAuto = 38;
constexpr int kCtxDrawIndexedInstancedIndirect = 39;
constexpr int kCtxDrawInstancedIndirect = 40;
constexpr int kCtxExecuteCommandList = 58;
constexpr int kCtxCopySubresourceRegion = 46;
constexpr int kCtxPSSetShaderResources = 8;
constexpr int kCtxCopyResource = 47;
constexpr int kSwapPresent = 8;

using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_RSSetViewports = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using PFN_RSSetScissorRects = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
using PFN_DrawAuto = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PFN_DrawIndirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN_ExecuteCommandList = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PFN_CopySubresourceRegion = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                                           ID3D11Resource*, UINT, const D3D11_BOX*);
using PFN_CopyResource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using PFN_PSSetShaderResources = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                          ID3D11ShaderResourceView* const*);
using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                        ID3D11DepthStencilView*);
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

PFN_DrawIndexed g_realDrawIndexed = nullptr;
PFN_Draw g_realDraw = nullptr;
PFN_DrawIndexedInstanced g_realDrawIndexedInstanced = nullptr;
PFN_DrawInstanced g_realDrawInstanced = nullptr;
PFN_RSSetViewports g_realRSSetViewports = nullptr;
PFN_RSSetScissorRects g_realRSSetScissorRects = nullptr;
PFN_OMSetRenderTargets g_realOMSetRenderTargets = nullptr;
PFN_DrawAuto g_realDrawAuto = nullptr;
PFN_DrawIndirect g_realDrawIndexedInstancedIndirect = nullptr;
PFN_DrawIndirect g_realDrawInstancedIndirect = nullptr;
PFN_ExecuteCommandList g_realExecuteCommandList = nullptr;
PFN_CopySubresourceRegion g_realCopySubresourceRegion = nullptr;
PFN_CopyResource g_realCopyResource = nullptr;
PFN_PSSetShaderResources g_realPSSetShaderResources = nullptr;
// Draw kinds not otherwise tracked, and draws from threads other than the presenting one (per second).
std::atomic<uint32_t> g_otherDrawKindsSecond{0}, g_commandListsSecond{0}, g_otherThreadDrawsSecond{0};
DWORD g_presentThread = 0;
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
Hist g_unlistedCallers, g_unlistedLoop; // draws outside the listed passes while VR runs
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

// First exe return address (the renderer function right above D3D) and first main-loop one, into the two histograms.
void SampleCallers(uintptr_t esp, Hist& directHist, Hist& loopHist)
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
            directHist.Add(v);
            haveDirect = true;
        }
        if (!haveLoop && v >= kMainLoopBegin && v < kMainLoopEnd)
        {
            loopHist.Add(v);
            haveLoop = true;
        }
    }
}

void SampleDrawCallers(uintptr_t esp) { SampleCallers(esp, g_directCallers, g_loopCallers); }

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

// ---- VR HUD panel ----
// The HUD is drawn by the GUI pass, Scene_RenderFrame's last call: 00808F31 PUSH 0; CALL FUN_0079C480 (return
// 00808F38), with the 2D renderer reusing the engine's cached full-screen viewport -- the pass itself sets no D3D
// viewport (2026-10-16: a squeeze on RSSetViewports inside the pass never fired). So the pass is detected at draw time:
//   - Frame_RenderScene (0080A440) calls Scene_RenderFrame; its return address 0080A44B sits at a fixed stack slot A
//     for each of the two renders (the game's own, and the right eye's from the Present hook, which runs deeper).
//   - During the GUI call the stack reads [A-12] = 00808F38, [A-8] = 0 (its argument), [A-4] = saved EDI, [A] = 0080A44B.
// A is learned once per render by a stack scan (the game's render: the shallowest match, since stale copies left by the
// right-eye render lie deeper; the right-eye render: the deepest), after that each draw reads three dwords.
// The first GUI draw "latches" the HUD state until ResetHudViewport (after each eye's copy): while the bound render
// target is the eye image (the scene target or back buffer), the game's full-screen viewport (and scissors) are
// squeezed into stereo::HudPanel(). Offscreen targets keep the game's viewport: the motion tracker is drawn into one
// (2026-10-17: squeezing it too left a frozen copy of the tracker -- its new content landed in a corner of the texture,
// the HUD kept showing the never-cleared old content).
constexpr uint32_t kGuiPassReturn = 0x00808F38;
constexpr uint32_t kRenderSceneReturn = 0x0080A44B;
constexpr uint32_t kSceneRenderBegin = 0x00808E00, kSceneRenderEnd = 0x00808F52;

bool g_secondRender = false;      // the right eye's render (stereo.cpp, via SetSecondRender)
uintptr_t g_frameSlot[2] = {};    // A per render (0 = not learned)
DWORD g_lastLearn[2] = {};
bool g_hudLatched = false;        // GUI pass reached in the current render
uint32_t g_hudLatches = 0, g_hudLatchesSecond = 0, g_hudSqueezedViewportsSecond = 0;
uint32_t g_passDrawsSecond[kHudPassCount] = {}; // draws per late pass this second (eye renders only)

D3D11_VIEWPORT g_hudOrigViewport{}; // while latched: the viewport the game asked for (unsqueezed)
bool g_hudViewportSqueezed = false; // the bound viewport is the squeezed one
bool g_hudMainTarget = false;       // while latched: the bound render target is the eye image
uint32_t g_hudDrawsMainSecond = 0, g_hudDrawsOffscreenSecond = 0;
D3D11_RECT g_hudOrigScissor{};
bool g_hudScissorSqueezed = false;

bool InSceneRender(uint32_t v) { return v >= kSceneRenderBegin && v < kSceneRenderEnd; }

uintptr_t LearnFrameSlot(uintptr_t esp, bool deepest)
{
    uintptr_t found = 0;
    __try
    {
        const auto* p = reinterpret_cast<const volatile uint32_t*>(esp);
        for (int i = 3; i < 16384; ++i) // up to 64 KB; reading past the stack top ends the scan
        {
            if (p[i] != kRenderSceneReturn)
                continue;
            if (!InSceneRender(p[i - 1]) && !InSceneRender(p[i - 2]) && !InSceneRender(p[i - 3]))
                continue;
            found = esp + i * 4;
            if (deepest)
                break;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return found;
}

int FindPass(int slot, uint32_t returnAddress)
{
    for (int j = 0; j < kHudPassCount; ++j)
        if (kHudPasses[j].slot == slot && kHudPasses[j].returnAddress == returnAddress)
            return j;
    return -1;
}

// The Scene_RenderFrame call running now, as an index into kHudPasses; -1 = none listed; -2 = A doesn't hold
// Frame_RenderScene's return address (not inside Scene_RenderFrame, or A is stale). Slots are checked from A down, so a
// saved register or argument above a later call's return address is seen first (none of those is a code address).
int PassAtSlot(uintptr_t a)
{
    uint32_t ret = 0, v4 = 0, v8 = 0, v12 = 0, v16 = 0;
    if (!SafeReadStack(a, &ret) || ret != kRenderSceneReturn)
        return -2;
    SafeReadStack(a - 4, &v4);
    SafeReadStack(a - 8, &v8);
    SafeReadStack(a - 12, &v12);
    SafeReadStack(a - 16, &v16);
    if (InSceneRender(v4))
        return FindPass(-4, v4); // a call before 00808E93's PUSH EDI
    if (InSceneRender(v8))
        return FindPass(-8, v8);
    if (v8 == 0 && v12 == kGuiPassReturn)
        return kGuiPass;
    if (InSceneRender(v16))
        return FindPass(-16, v16);
    return -1;
}
int CurrentHudPass(uintptr_t esp)
{
    const int r = g_secondRender ? 1 : 0;
    if (g_frameSlot[r])
    {
        const int pass = PassAtSlot(g_frameSlot[r]);
        if (pass != -2)
            return pass;
    }
    // Unknown, or this draw isn't inside Scene_RenderFrame: (re)learn, at most twice a second per render.
    const DWORD now = GetTickCount();
    if (now - g_lastLearn[r] < 500)
        return -1;
    g_lastLearn[r] = now;
    const uintptr_t learned = LearnFrameSlot(esp, r == 1);
    if (!learned || learned == g_frameSlot[r])
        return -1;
    g_frameSlot[r] = learned;
    AVP_LOG(kLog, "HUD: %s render's Frame_RenderScene return slot at %08X", r ? "right-eye" : "game",
            static_cast<unsigned>(learned));
    const int pass = PassAtSlot(learned);
    return pass < 0 ? -1 : pass;
}
bool IsFullTarget(const D3D11_VIEWPORT& v)
{
    return v.TopLeftX == 0.0f && v.TopLeftY == 0.0f &&
           v.Width == static_cast<float>(Global<int>(addr::g_BackBufferWidth)) &&
           v.Height == static_cast<float>(Global<int>(addr::g_BackBufferHeight));
}

void SqueezeViewport(const float* p, D3D11_VIEWPORT& v)
{
    v.TopLeftX += v.Width * p[2];
    v.TopLeftY += v.Height * p[3];
    v.Width *= p[0];
    v.Height *= p[1];
}

void SqueezeRect(const float* p, D3D11_RECT& r)
{
    const float w = static_cast<float>(Global<int>(addr::g_BackBufferWidth));
    const float h = static_cast<float>(Global<int>(addr::g_BackBufferHeight));
    r.left = static_cast<LONG>(std::floor(r.left * p[0] + w * p[2]));
    r.right = static_cast<LONG>(std::ceil(r.right * p[0] + w * p[2]));
    r.top = static_cast<LONG>(std::floor(r.top * p[1] + h * p[3]));
    r.bottom = static_cast<LONG>(std::ceil(r.bottom * p[1] + h * p[3]));
}

bool IsMainTargetView(ID3D11RenderTargetView* rtv)
{
    if (!rtv)
        return false;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    const auto scene = Global<uintptr_t>(addr::g_SceneTarget), back = Global<uintptr_t>(addr::g_BackBuffer);
    const bool main = res && (reinterpret_cast<uintptr_t>(res) == scene || reinterpret_cast<uintptr_t>(res) == back);
    if (res)
        res->Release();
    return main;
}

// The first few distinct offscreen targets the GUI pass draws into (once each).
void LogOffscreenTarget(ID3D11RenderTargetView* rtv)
{
    static uintptr_t seen[8];
    static int seenCount = 0;
    if (!rtv || seenCount >= 8)
        return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (!res)
        return;
    const auto key = reinterpret_cast<uintptr_t>(res);
    bool known = false;
    for (int i = 0; i < seenCount && !known; ++i)
        known = seen[i] == key;
    if (!known)
    {
        seen[seenCount++] = key;
        ID3D11Texture2D* tex = nullptr;
        D3D11_TEXTURE2D_DESC d{};
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
        {
            tex->GetDesc(&d);
            tex->Release();
        }
        AVP_LOG(kLog, "HUD: GUI pass draws into offscreen target %p: %ux%u format %u (game viewport %.0f,%.0f %.0fx%.0f)",
                res, d.Width, d.Height, d.Format, g_hudOrigViewport.TopLeftX, g_hudOrigViewport.TopLeftY,
                g_hudOrigViewport.Width, g_hudOrigViewport.Height);
    }
    res->Release();
}

// While latched: bind the squeezed viewport (eye image, full-screen game viewport) or the game's own.
void ApplyHudViewport(ID3D11DeviceContext* c)
{
    const float* panel = stereo::HudPanel();
    const bool squeeze = panel && g_hudMainTarget && IsFullTarget(g_hudOrigViewport);
    if (squeeze)
    {
        D3D11_VIEWPORT v = g_hudOrigViewport;
        SqueezeViewport(panel, v);
        g_realRSSetViewports(c, 1, &v);
        if (!g_hudViewportSqueezed)
            ++g_hudSqueezedViewportsSecond;
    }
    else if (g_hudViewportSqueezed)
    {
        g_realRSSetViewports(c, 1, &g_hudOrigViewport);
    }
    g_hudViewportSqueezed = squeeze;
}

// ---- Debug: hide a pass's draws (settings DebugHidePass / DebugHideDraw) ----
int g_curPass = -1;          // pass of the previous draw in this eye render
uint32_t g_drawInPass = 0;   // 1-based number of the current draw within its pass
uint32_t g_hiddenSecond = 0; // draws skipped this second

// What a single hidden draw is, so it can be recognised without the debug rows: on each new selection, then every 3 s.
void LogHiddenDraw(ID3D11DeviceContext* c, int pass, int drawNo, UINT count)
{
    static int lastPass = -1, lastDraw = -1;
    static DWORD lastTime = 0;
    const DWORD now = GetTickCount();
    if (pass == lastPass && drawNo == lastDraw && now - lastTime < 3000)
        return;
    lastPass = pass;
    lastDraw = drawNo;
    lastTime = now;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    c->VSGetShader(&vs, nullptr, nullptr);
    c->PSGetShader(&ps, nullptr, nullptr);
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    c->IAGetPrimitiveTopology(&topo);
    D3D11_TEXTURE2D_DESC tex{};
    ID3D11ShaderResourceView* srv = nullptr;
    c->PSGetShaderResources(0, 1, &srv);
    if (srv)
    {
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        ID3D11Texture2D* t2 = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t2))) && t2)
        {
            t2->GetDesc(&tex);
            t2->Release();
        }
        if (res)
            res->Release();
        srv->Release();
    }
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    const bool main = IsMainTargetView(rtv);
    if (rtv)
        rtv->Release();
    UINT n = 1;
    D3D11_VIEWPORT vp{};
    c->RSGetViewports(&n, &vp);
    AVP_LOG(kLog,
            "hidden: pass '%s' draw #%d (%s render): %u indices/vertices, topology %d, VS %p PS %p, PS texture 0 "
            "%ux%u format %u, target %s, viewport %.0f,%.0f %.0fx%.0f",
            HudPassLabel(pass), drawNo, g_secondRender ? "right-eye" : "game", count, topo, vs, ps, tex.Width,
            tex.Height, tex.Format, main ? "eye image" : "offscreen", vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height);
    if (vs)
        vs->Release();
    if (ps)
        ps->Release();
}

// Before every draw while an eye renders: identify the pass, skip the draw if the debug rows say so (returns true),
// count; on the first GUI-pass draw, latch the HUD squeeze.
// Draws outside the listed passes while VR runs: where they come from, which viewports / targets (per second).
uint32_t g_unlistedSecond = 0, g_unlistedOutsideEyeSecond = 0, g_unlistedSample = 0;
struct UnlistedKind
{
    float w, h;
    bool mainTarget, inEyeRender;
    uint32_t count;
};
UnlistedKind g_unlistedKinds[8];
int g_unlistedKindCount = 0;

void NoteUnlistedDraw(ID3D11DeviceContext* c, uintptr_t esp, bool inEye)
{
    ++g_unlistedSecond;
    if (!inEye)
        ++g_unlistedOutsideEyeSecond;
    if ((g_unlistedSample++ & 3) != 0)
        return; // details for every 4th
    SampleCallers(esp, g_unlistedCallers, g_unlistedLoop);
    UINT n = 1;
    D3D11_VIEWPORT vp{};
    c->RSGetViewports(&n, &vp);
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    const bool main = IsMainTargetView(rtv);
    if (rtv)
        rtv->Release();
    for (int i = 0; i < g_unlistedKindCount; ++i)
    {
        UnlistedKind& k = g_unlistedKinds[i];
        if (k.w == vp.Width && k.h == vp.Height && k.mainTarget == main && k.inEyeRender == inEye)
        {
            ++k.count;
            return;
        }
    }
    if (g_unlistedKindCount < 8)
        g_unlistedKinds[g_unlistedKindCount++] = {vp.Width, vp.Height, main, inEye, 1};
}

// ---- The duplicate motion tracker (settings HideDuplicateTracker) ----
// The marine's HUD tracker is 2 draws per frame outside the listed passes (2026-10-17 log: 18 indices with a 512x512
// texture, 6 with a 128x128 one, queued by Scene_RenderFrame's tail call 006EF6A0 -> 006C8810 and flushed by the 2D
// batch). A second, live tracker without the objective arrow -- some other draw sampling the same texture, placed for
// the flat screen -- stayed at its full-screen spot outside the panel; hiding every other draw that samples those
// textures removes exactly it (user-verified). The textures are learned from the HUD's own draws (marine missions only:
// g_PlayerSpecies 2 runs the tracker) and forgotten whenever VR gameplay stops (a level change can reuse the views).
// Which ones are bound is tracked in the PSSetShaderResources hook by pointer, so the per-draw test is one mask check.
constexpr int kTrackedSlots = 8;
ID3D11ShaderResourceView* g_trackerSrv[4] = {}; // compared by address only, never used (no reference held)
int g_trackerSrvCount = 0;
uint32_t g_trackerBoundMask = 0; // PS slots 0-7 currently holding one of them
uint32_t g_trackerHiddenSecond = 0;

bool IsTrackerSrv(ID3D11ShaderResourceView* v)
{
    for (int i = 0; i < g_trackerSrvCount; ++i)
        if (v && g_trackerSrv[i] == v)
            return true;
    return false;
}

void ForgetTrackerTextures()
{
    if (g_trackerSrvCount)
        AVP_LOG(kLog, "TRACKER: forgot %d texture(s)", g_trackerSrvCount);
    g_trackerSrvCount = 0;
    g_trackerBoundMask = 0;
}

void RecomputeTrackerMask(ID3D11DeviceContext* c)
{
    ID3D11ShaderResourceView* srvs[kTrackedSlots] = {};
    c->PSGetShaderResources(0, kTrackedSlots, srvs);
    g_trackerBoundMask = 0;
    for (int s = 0; s < kTrackedSlots; ++s)
    {
        if (IsTrackerSrv(srvs[s]))
            g_trackerBoundMask |= 1u << s;
        if (srvs[s])
            srvs[s]->Release();
    }
}

// At one of the HUD's own tracker draws: remember the texture in slot 0.
void LearnTrackerTexture(ID3D11DeviceContext* c)
{
    if (g_trackerSrvCount >= 4)
        return;
    ID3D11ShaderResourceView* srv = nullptr;
    c->PSGetShaderResources(0, 1, &srv);
    if (!srv)
        return;
    const bool known = IsTrackerSrv(srv);
    if (!known)
    {
        g_trackerSrv[g_trackerSrvCount++] = srv;
        D3D11_TEXTURE2D_DESC d{};
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
        {
            tex->GetDesc(&d);
            tex->Release();
        }
        if (res)
            res->Release();
        AVP_LOG(kLog, "TRACKER: HUD tracker texture %p (%ux%u format %u)", srv, d.Width, d.Height, d.Format);
    }
    srv->Release();
    if (!known)
        RecomputeTrackerMask(c);
}
// ---- Copies into the eye image (scene target / back buffer), per second ----
struct CopyKind
{
    bool region;
    uintptr_t src;
    UINT x, y, srcW, srcH;
    uint32_t count;
};
CopyKind g_copyKinds[8];
int g_copyKindCount = 0;

bool IsMainResource(ID3D11Resource* r)
{
    const auto p = reinterpret_cast<uintptr_t>(r);
    return p && (p == Global<uintptr_t>(addr::g_SceneTarget) || p == Global<uintptr_t>(addr::g_BackBuffer));
}

void NoteCopyIntoMain(bool region, ID3D11Resource* src, UINT x, UINT y)
{
    const auto key = reinterpret_cast<uintptr_t>(src);
    for (int i = 0; i < g_copyKindCount; ++i)
        if (g_copyKinds[i].region == region && g_copyKinds[i].src == key && g_copyKinds[i].x == x && g_copyKinds[i].y == y)
        {
            ++g_copyKinds[i].count;
            return;
        }
    if (g_copyKindCount >= 8)
        return;
    D3D11_TEXTURE2D_DESC d{};
    ID3D11Texture2D* tex = nullptr;
    if (src && SUCCEEDED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
    {
        tex->GetDesc(&d);
        tex->Release();
    }
    g_copyKinds[g_copyKindCount++] = {region, key, x, y, d.Width, d.Height, 1};
}

bool HudBeforeDraw(ID3D11DeviceContext* c, uintptr_t esp, UINT count)
{
    const bool inEye = stereo::InEyeRender();
    if (!inEye && !stereo::StereoActive())
    {
        if (g_trackerSrvCount)
            ForgetTrackerTextures(); // VR gameplay stopped (menu, loading): the next level has new textures
        return false;
    }
    const int pass = inEye ? CurrentHudPass(esp) : -1;
    const int passKey = pass >= 0 ? pass : kHidePassUnlisted;
    if (passKey != g_curPass)
    {
        g_curPass = passKey;
        g_drawInPass = 0;
    }
    ++g_drawInPass;
    if (pass >= 0)
        ++g_passDrawsSecond[pass];
    else
        NoteUnlistedDraw(c, esp, inEye);

    if (settings::HideDuplicateTracker() && Global<int>(addr::g_PlayerSpecies) == 2)
    {
        if (pass < 0 && inEye)
            LearnTrackerTexture(c); // the HUD's own tracker draws (kept)
        else if (g_trackerBoundMask)
        {
            ++g_trackerHiddenSecond;
            return true; // the duplicate
        }
    }
    else if (g_trackerSrvCount)
    {
        ForgetTrackerTextures();
    }

    const int hidePass = settings::DebugHidePass();
    if (hidePass >= 0 && passKey == hidePass)
    {
        const int hideDraw = settings::DebugHideDraw();
        if (hideDraw == 0 || g_drawInPass == static_cast<uint32_t>(hideDraw))
        {
            ++g_hiddenSecond;
            if (hideDraw != 0)
                LogHiddenDraw(c, passKey, hideDraw, count);
            return true;
        }
    }

    // Marine reticle (settings HideMarineReticle): the GUI pass's 9th draw (user-found with the debug rows,
    // 2026-10-17; not the reticle for the Alien or Predator). Its signature is logged for a sturdier match later.
    if (pass == kGuiPass && g_drawInPass == 9 && settings::HideMarineReticle() && Global<int>(addr::g_PlayerSpecies) == 2)
    {
        static int logged = 0;
        if (logged < 3)
        {
            ++logged;
            LogHiddenDraw(c, passKey, 9, count);
        }
        return true;
    }

    if (!inEye)
        return false;
    if (g_hudLatched)
    {
        ++(g_hudMainTarget ? g_hudDrawsMainSecond : g_hudDrawsOffscreenSecond);
        return false;
    }
    if (pass != kGuiPass || !stereo::HudPanel())
        return false;
    g_hudLatched = true;
    ++g_hudLatchesSecond;
    UINT n = 1;
    c->RSGetViewports(&n, &g_hudOrigViewport);
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    g_hudMainTarget = IsMainTargetView(rtv);
    if (!g_hudMainTarget)
        LogOffscreenTarget(rtv);
    if (rtv)
        rtv->Release();
    if (g_hudLatches++ < 6)
        AVP_LOG(kLog, "HUD: GUI pass reached (%s render), viewport %.0f,%.0f %.0fx%.0f, target %s",
                g_secondRender ? "right-eye" : "game", g_hudOrigViewport.TopLeftX, g_hudOrigViewport.TopLeftY,
                g_hudOrigViewport.Width, g_hudOrigViewport.Height, g_hudMainTarget ? "eye image" : "offscreen");
    ApplyHudViewport(c);
    ++(g_hudMainTarget ? g_hudDrawsMainSecond : g_hudDrawsOffscreenSecond);
    return false;
}
void STDMETHODCALLTYPE Hook_OMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs,
                                               ID3D11DepthStencilView* dsv)
{
    g_realOMSetRenderTargets(c, n, rtvs, dsv);
    if (!g_hudLatched)
        return;
    ID3D11RenderTargetView* rtv = n > 0 && rtvs ? rtvs[0] : nullptr;
    g_hudMainTarget = IsMainTargetView(rtv);
    if (!g_hudMainTarget)
        LogOffscreenTarget(rtv);
    ApplyHudViewport(c);
}
// true = skip the draw (debug hide)
inline bool CountDraw(ID3D11DeviceContext* c, uintptr_t esp, UINT count)
{
    if (g_presentThread && GetCurrentThreadId() != g_presentThread)
    {
        g_otherThreadDrawsSecond.fetch_add(1, std::memory_order_relaxed);
        return false; // only the presenting thread's draws are counted / squeezed / hidden
    }
    ++g_frame.draws;
    const int view = Global<int>(addr::g_CurViewIndex);
    ++g_frame.drawsView[view == 0 ? 0 : view == 1 ? 1 : 2];
    if ((++g_drawSample & 31) == 0)
        SampleDrawCallers(esp);
    return HudBeforeDraw(c, esp, count);
}

#define DRAW_ESP reinterpret_cast<uintptr_t>(_AddressOfReturnAddress())

void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* c, UINT count, UINT start, INT base)
{
    if (!CountDraw(c, DRAW_ESP, count))
        g_realDrawIndexed(c, count, start, base);
}
void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* c, UINT count, UINT start)
{
    if (!CountDraw(c, DRAW_ESP, count))
        g_realDraw(c, count, start);
}
void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f)
{
    if (!CountDraw(c, DRAW_ESP, a))
        g_realDrawIndexedInstanced(c, a, b, d, e, f);
}
void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e)
{
    if (!CountDraw(c, DRAW_ESP, a))
        g_realDrawInstanced(c, a, b, d, e);
}

void STDMETHODCALLTYPE Hook_DrawAuto(ID3D11DeviceContext* c)
{
    g_otherDrawKindsSecond.fetch_add(1, std::memory_order_relaxed);
    g_realDrawAuto(c);
}
void STDMETHODCALLTYPE Hook_DrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o)
{
    g_otherDrawKindsSecond.fetch_add(1, std::memory_order_relaxed);
    g_realDrawIndexedInstancedIndirect(c, b, o);
}
void STDMETHODCALLTYPE Hook_DrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o)
{
    g_otherDrawKindsSecond.fetch_add(1, std::memory_order_relaxed);
    g_realDrawInstancedIndirect(c, b, o);
}
void STDMETHODCALLTYPE Hook_ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* list, BOOL restore)
{
    g_commandListsSecond.fetch_add(1, std::memory_order_relaxed);
    g_realExecuteCommandList(c, list, restore);
}

void STDMETHODCALLTYPE Hook_PSSetShaderResources(ID3D11DeviceContext* c, UINT start, UINT n,
                                                 ID3D11ShaderResourceView* const* views)
{
    g_realPSSetShaderResources(c, start, n, views);
    if (!g_trackerSrvCount || reinterpret_cast<uintptr_t>(c) != Global<uintptr_t>(addr::g_D3DContext))
        return;
    for (UINT i = 0; i < n; ++i)
    {
        const UINT slot = start + i;
        if (slot >= static_cast<UINT>(kTrackedSlots))
            break;
        if (views && IsTrackerSrv(views[i]))
            g_trackerBoundMask |= 1u << slot;
        else
            g_trackerBoundMask &= ~(1u << slot);
    }
}

void STDMETHODCALLTYPE Hook_CopyResource(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src)
{
    if (IsMainResource(dst) && stereo::StereoActive() && GetCurrentThreadId() == g_presentThread)
        NoteCopyIntoMain(false, src, 0, 0);
    g_realCopyResource(c, dst, src);
}
void STDMETHODCALLTYPE Hook_CopySubresourceRegion(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT dstSub, UINT x,
                                                  UINT y, UINT z, ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box)
{
    if (IsMainResource(dst) && stereo::StereoActive() && GetCurrentThreadId() == g_presentThread)
        NoteCopyIntoMain(true, src, x, y);
    g_realCopySubresourceRegion(c, dst, dstSub, x, y, z, src, srcSub, box);
}

void STDMETHODCALLTYPE Hook_RSSetScissorRects(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* rects)
{
    g_hudScissorSqueezed = false;
    const float* panel = g_hudLatched ? stereo::HudPanel() : nullptr;
    if (panel && g_hudViewportSqueezed && n == 1 && rects)
    {
        g_hudOrigScissor = rects[0];
        D3D11_RECT r = rects[0];
        SqueezeRect(panel, r);
        g_hudScissorSqueezed = true;
        g_realRSSetScissorRects(c, 1, &r);
        return;
    }
    g_realRSSetScissorRects(c, n, rects);
}

void STDMETHODCALLTYPE Hook_RSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* vps)
{
    ++g_frame.viewports;
    if (g_hudLatched && n == 1 && vps)
    {
        g_hudOrigViewport = vps[0];
        g_hudViewportSqueezed = true; // force ApplyHudViewport to bind one or the other
        ApplyHudViewport(c);
        return;
    }
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
    g_presentThread = GetCurrentThreadId();
    static bool haveWindow = false;
    if (!haveWindow)
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(sc->GetDesc(&desc)) && desc.OutputWindow)
        {
            SetGameWindow(desc.OutputWindow);
            haveWindow = true;
        }
    }
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
        {
            char line[1536];
            int used = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                   "  HUD: latched %u times, %u viewports squeezed, GUI draws/frame into the eye image "
                                   "%.1f / offscreen %.1f; debug-hidden %u; pass draws/frame (both eyes):",
                                   g_hudLatchesSecond, g_hudSqueezedViewportsSecond, g_hudDrawsMainSecond / f,
                                   g_hudDrawsOffscreenSecond / f, g_hiddenSecond);
            for (int j = 0; j < kHudPassCount; ++j)
                if (g_passDrawsSecond[j] && used > 0 && used < static_cast<int>(sizeof(line)))
                    used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, " [%s] %.1f", kHudPasses[j].label,
                                        g_passDrawsSecond[j] / f);
            AVP_LOG(kLog, "%s", line);
        }
        g_hudLatchesSecond = g_hudSqueezedViewportsSecond = g_hudDrawsMainSecond = g_hudDrawsOffscreenSecond = 0;
        g_hiddenSecond = 0;
        if (g_unlistedSecond || g_otherThreadDrawsSecond || g_otherDrawKindsSecond || g_commandListsSecond)
        {
            char line[1024];
            int used = _snprintf_s(line, sizeof(line), _TRUNCATE,
                                   "  unlisted draws/frame %.1f (outside eye renders %.1f); per second: %u draws on other "
                                   "threads, %u indirect/auto draws, %u command lists; kinds (viewport, target, in eye "
                                   "render, sampled 1/4):",
                                   g_unlistedSecond / f, g_unlistedOutsideEyeSecond / f, g_otherThreadDrawsSecond.load(),
                                   g_otherDrawKindsSecond.load(), g_commandListsSecond.load());
            for (int i = 0; i < g_unlistedKindCount && used > 0 && used < static_cast<int>(sizeof(line)); ++i)
            {
                const UnlistedKind& k = g_unlistedKinds[i];
                used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, " [%.0fx%.0f %s %s x%u]", k.w, k.h,
                                    k.mainTarget ? "eye image" : "offscreen", k.inEyeRender ? "in" : "out", k.count);
            }
            AVP_LOG(kLog, "%s", line);
            if (g_unlistedSecond)
            {
                LogHist("unlisted draw callers", g_unlistedCallers);
                LogHist("unlisted draws' main-loop frames", g_unlistedLoop);
            }
        }
        if (g_trackerHiddenSecond)
            AVP_LOG(kLog, "  TRACKER: duplicate motion tracker hidden, %.1f draws/frame (%d texture(s) known)",
                    g_trackerHiddenSecond / f, g_trackerSrvCount);
        g_trackerHiddenSecond = 0;
        if (g_copyKindCount)
        {
            char line[1024];
            int used = _snprintf_s(line, sizeof(line), _TRUNCATE, "  copies into the eye image:");
            for (int i = 0; i < g_copyKindCount && used > 0 && used < static_cast<int>(sizeof(line)); ++i)
            {
                const CopyKind& k = g_copyKinds[i];
                used += _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, " [%s from %p %ux%u at %u,%u x%u]",
                                    k.region ? "region" : "whole", reinterpret_cast<void*>(k.src), k.srcW, k.srcH, k.x,
                                    k.y, k.count);
            }
            AVP_LOG(kLog, "%s", line);
        }
        g_copyKindCount = 0;
        g_unlistedSecond = g_unlistedOutsideEyeSecond = 0;
        g_unlistedKindCount = 0;
        g_otherThreadDrawsSecond = g_otherDrawKindsSecond = g_commandListsSecond = 0;
        for (uint32_t& n : g_passDrawsSecond)
            n = 0;
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
            HookVtableEntry(ctx, kCtxRSSetScissorRects, reinterpret_cast<void*>(&Hook_RSSetScissorRects),
                            &g_realRSSetScissorRects, "RSSetScissorRects");
            HookVtableEntry(ctx, kCtxPSSetShaderResources, reinterpret_cast<void*>(&Hook_PSSetShaderResources),
                            &g_realPSSetShaderResources, "PSSetShaderResources");
            HookVtableEntry(ctx, kCtxCopyResource, reinterpret_cast<void*>(&Hook_CopyResource), &g_realCopyResource,
                            "CopyResource");
            HookVtableEntry(ctx, kCtxCopySubresourceRegion, reinterpret_cast<void*>(&Hook_CopySubresourceRegion),
                            &g_realCopySubresourceRegion, "CopySubresourceRegion");
            HookVtableEntry(ctx, kCtxDrawAuto, reinterpret_cast<void*>(&Hook_DrawAuto), &g_realDrawAuto, "DrawAuto");
            HookVtableEntry(ctx, kCtxDrawIndexedInstancedIndirect,
                            reinterpret_cast<void*>(&Hook_DrawIndexedInstancedIndirect),
                            &g_realDrawIndexedInstancedIndirect, "DrawIndexedInstancedIndirect");
            HookVtableEntry(ctx, kCtxDrawInstancedIndirect, reinterpret_cast<void*>(&Hook_DrawInstancedIndirect),
                            &g_realDrawInstancedIndirect, "DrawInstancedIndirect");
            HookVtableEntry(ctx, kCtxExecuteCommandList, reinterpret_cast<void*>(&Hook_ExecuteCommandList),
                            &g_realExecuteCommandList, "ExecuteCommandList");
            HookVtableEntry(ctx, kCtxOMSetRenderTargets, reinterpret_cast<void*>(&Hook_OMSetRenderTargets),
                            &g_realOMSetRenderTargets, "OMSetRenderTargets");
            HookVtableEntry(swap, kSwapPresent, reinterpret_cast<void*>(&Hook_Present), &g_realPresent, "Present");
            return;
        }
        Sleep(500);
    }
    AVP_LOG(kLog, "gave up waiting for the game's D3D11 context/swapchain");
}

} // namespace

void ResetHudViewport(ID3D11DeviceContext* ctx)
{
    if (g_hudLatched && g_hudViewportSqueezed && g_realRSSetViewports)
        g_realRSSetViewports(ctx, 1, &g_hudOrigViewport);
    if (g_hudScissorSqueezed && g_realRSSetScissorRects)
        g_realRSSetScissorRects(ctx, 1, &g_hudOrigScissor);
    g_hudViewportSqueezed = g_hudScissorSqueezed = false;
    g_hudLatched = false;
    g_curPass = -1; // the next eye render starts counting its passes afresh
}

void SetSecondRender(bool on) { g_secondRender = on; }

void StartD3DProbe()
{
    std::thread(ProbeThread).detach();
}

} // namespace avp
