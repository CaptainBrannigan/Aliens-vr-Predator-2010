#include "stereo.h"

#include "head_aim.h"
#include "openvr_session.h"
#include "../core/addresses.h"
#include "../core/dev_flags.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../hooks/d3d_probe.h"
#include "../hooks/vmt_shadow.h"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstring>

namespace avp::stereo {
namespace {

constexpr const char* kLog = "avpvr_stereo.log";

// Game functions (all cdecl, verified: plain RET) and the camera apply (thiscall(this, entity), RET 4).
using PFN_SetVec = void(__cdecl*)(float* v);
using PFN_SetFov = void(__cdecl*)(float fullAngle, float aspectScale);
using PFN_BeginFrame = void(__cdecl*)(char clear);
using PFN_RenderScene = void(__cdecl*)();
using PFN_CamApply = void(__thiscall*)(void* self, void* entity);

const auto SetPosition = reinterpret_cast<PFN_SetVec>(addr::Cam_SetPosition);
const auto SetRotation = reinterpret_cast<PFN_SetVec>(addr::Cam_SetRotationMatrix);
const auto SetHorizontalFov = reinterpret_cast<PFN_SetFov>(addr::Cam_SetHorizontalFov);
const auto BeginFrame = reinterpret_cast<PFN_BeginFrame>(addr::Frame_BeginFrame);
const auto RenderScene = reinterpret_cast<PFN_RenderScene>(addr::Frame_RenderScene);
const auto OriginalCamApply = reinterpret_cast<PFN_CamApply>(addr::CamComp_ApplyToRenderCamera);

constexpr uintptr_t kBaseAspect = 0x009CCD54; // float multiplied by the FOV setters' aspect scale

// ---- Per-frame state (main thread only) ----
bool g_stereoFrame = false;   // left eye camera applied this frame, right eye pending at Present
bool g_posesWaited = false;   // WaitGetPoses already called for this frame
vr::Pose g_head{};
ViewCamera g_base;            // the game's own camera this frame
bool g_haveRefYaw = false;
std::atomic<bool> g_recenterRequested{false}; // dashboard button (any thread)
unsigned g_recenterCount = 0;                  // recenters done (main thread)
DWORD g_presentThread = 0;
float g_refYaw = 0.0f;        // head yaw at the first stereo frame (recenter reference)
unsigned g_stereoFrames = 0;
float g_gameAspect = 0.0f;         // the game's global aspect (009CCD54) before VR changed it
float g_lastWrittenAspect = -1.0f; // what VR last wrote there

ID3D11Texture2D* g_eyeTex[2] = {};
ID3D11Texture2D* g_screenTex = nullptr;
ID3D11Texture2D* g_blackTex = nullptr;
UINT g_texW = 0, g_texH = 0;

struct EyeCamera
{
    float pos[3];
    float rot[9];
    float fullHFov;
    float aspect; // tanH / tanV
    float bounds[4]; // uMin vMin uMax vMax
    float hudPanel[4]; // HUD panel in the eye's render: scaleX scaleY offsetX offsetY (fractions of the render)
};

// The HUD panel of the eye being rendered (HudPanel()), valid while g_hudPanelActive.
float g_hudPanel[4] = {1.0f, 1.0f, 0.0f, 0.0f};
bool g_hudPanelActive = false;
DWORD g_lastStereoTick = 0; // last stereo frame's camera apply

using PFN_IsPausedMenu = bool(__cdecl*)();
const auto IsPausedWithMenu = reinterpret_cast<PFN_IsPausedMenu>(addr::Game_IsPausedWithMenu);
const auto IsMenuScreenUp = reinterpret_cast<PFN_IsPausedMenu>(addr::Gui_IsMenuScreenUp);

// The pause-menu decision's inputs, logged whenever they change (multiplayer pause menu investigation).
void LogMenuState(bool pausedWithMenu)
{
    const bool menuUp = IsMenuScreenUp();
    const bool paused = Global<uint8_t>(addr::g_GamePaused) != 0;
    const float timeScale = Global<float>(addr::g_TimeScale);
    const bool block = Global<uint8_t>(addr::g_PauseMenuBlock) != 0;
    const int state = (menuUp ? 1 : 0) | (paused ? 2 : 0) | (timeScale <= 0.0f ? 4 : 0) | (block ? 8 : 0) |
                      (pausedWithMenu ? 16 : 0);
    static int last = -1;
    if (state == last)
        return;
    last = state;
    AVP_LOG(kLog, "menu state: GUI menu screen up %d, paused %d, time scale %.2f, block flag %d -> paused with menu %d",
            menuUp, paused, timeScale, block, pausedWithMenu);
}

// ---- Engine angle convention: (pitch, yaw, roll), exact copies of FUN_004EFD20 / Cam_SetRotationQuat ----
void EulerToQuat(const float a[3], float q[4])
{
    const float c0 = std::cos(a[0] * 0.5f), s0 = std::sin(a[0] * 0.5f);
    const float c1 = std::cos(a[1] * 0.5f), s1 = std::sin(a[1] * 0.5f);
    const float c2 = std::cos(a[2] * 0.5f), s2 = std::sin(a[2] * 0.5f);
    q[0] = c2 * c1 * s0 + s2 * s1 * c0;
    q[1] = c2 * s1 * c0 - s2 * c1 * s0;
    q[2] = s2 * c1 * c0 - c2 * s1 * s0;
    q[3] = c2 * c1 * c0 + s2 * s1 * s0;
}

void QuatToRot(const float q[4], float m[9])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float z2 = z * 2.0f, y2 = y * 2.0f, wx2 = w * x * 2.0f, zz2 = z * z2, xx2 = x * x * 2.0f;
    m[0] = 1.0f - (zz2 + y * y2);
    m[1] = x * y2 - w * z2;
    m[2] = x * z2 + w * y2;
    m[3] = x * y2 + w * z2;
    m[4] = 1.0f - (zz2 + xx2);
    m[5] = y * z2 - wx2;
    m[6] = x * z2 - w * y2;
    m[7] = y * z2 + wx2;
    m[8] = 1.0f - (y * y2 + xx2);
}

// Head yaw/pitch/roll from the OpenVR pose (forward = -Z column, up = +Y column, right = +X column).
void HeadAngles(const vr::Pose& p, float& yaw, float& pitch, float& roll)
{
    const float fx = -p.m[0][2], fy = -p.m[1][2], fz = -p.m[2][2];
    yaw = std::atan2(-fx, -fz);                                   // + = turned left
    pitch = std::asin(fy < -1.0f ? -1.0f : fy > 1.0f ? 1.0f : fy); // + = looking up
    roll = std::atan2(p.m[1][0], p.m[1][1]);                      // + = right side up (head tilted left)
}

// The head's rotation in the engine's convention, relative to the recenter reference: engine angles (head pitch,
// signed head yaw, signed head roll) -> the engine's own quaternion -> matrix (columns = right, up, forward).
void HeadMatrix(const vr::Pose& p, float m[9])
{
    float headYaw, headPitch, headRoll;
    HeadAngles(p, headYaw, headPitch, headRoll);
    const float angles[3] = {dev::kHeadPitchSign * headPitch, dev::kHeadYawSign * (headYaw - g_refYaw),
                             dev::kHeadRollSign * headRoll};
    float q[4];
    EulerToQuat(angles, q);
    QuatToRot(q, m);
}

// c = a * b (3x3, row-major).
void MatMul(const float a[9], const float b[9], float c[9])
{
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            c[r * 3 + k] = a[r * 3 + 0] * b[0 * 3 + k] + a[r * 3 + 1] * b[1 * 3 + k] + a[r * 3 + 2] * b[2 * 3 + k];
}

// m keeps its forward (column 2); its down axis (column 1) becomes ref's, made perpendicular, and right = down x
// forward (the engine's column convention). Falls back to m's own down axis if ref's is (nearly) along the forward.
void ReRoll(float m[9], const float ref[9])
{
    const float f[3] = {m[2], m[5], m[8]};
    float d[3] = {ref[1], ref[4], ref[7]};
    float k = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    d[0] -= k * f[0];
    d[1] -= k * f[1];
    d[2] -= k * f[2];
    float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-3f)
        return;
    for (float& v : d)
        v /= len;
    const float r[3] = {d[1] * f[2] - d[2] * f[1], d[2] * f[0] - d[0] * f[2], d[0] * f[1] - d[1] * f[0]};
    m[0] = r[0];
    m[3] = r[1];
    m[6] = r[2];
    m[1] = d[0];
    m[4] = d[1];
    m[7] = d[2];
}

// eye 0 = left, 1 = right.
EyeCamera BuildEye(int eye)
{
    EyeCamera c{};
    // Orientation. With head aim (vr/head_aim.h) the player frame P -- the body's up (the Alien's surface) and our own
    // heading -- times the head: view = P * head, the matrix that set the game's look this frame. With
    // GameCameraBase (default) the game's own camera stays authoritative: it was built from that look frame L, so its
    // difference D = G * L^T (camera shake, recoil, the Alien's own camera roll and smoothing) is kept on top:
    // camera = D * P * head. Without head aim (off, or before the first look update): the game's yaw + the head's
    // angles (first VR builds); that spins on Alien walls, where the game's camera isn't upright.
    float frame[9], look[9];
    if (aim::PlayerFrame(frame, look))
    {
        float head[9], view[9];
        HeadMatrix(g_head, head);
        MatMul(frame, head, view);
        if (settings::GameCameraBase())
        {
            const float lt[9] = {look[0], look[3], look[6], look[1], look[4], look[7], look[2], look[5], look[8]};
            float d[9];
            MatMul(g_base.rot, lt, d);
            MatMul(d, view, c.rot);
            // The game camera's own roll stays out (2026-10-17: the Alien has a roll input; same for every species
            // since): keep the game camera's direction, rebuild its roll from our view (columns right, down, forward;
            // right = down x forward).
            if (settings::RollFromHeadOnly())
                ReRoll(c.rot, view);
        }
        else
        {
            std::memcpy(c.rot, view, sizeof(view));
        }
    }
    else
    {
        float headYaw, headPitch, headRoll;
        HeadAngles(g_head, headYaw, headPitch, headRoll);
        const float relYaw = headYaw - g_refYaw;
        float angles[3];
        angles[0] = dev::kHeadPitchSign * headPitch + (dev::kAddGamePitch ? g_base.pitch : 0.0f);
        angles[1] = g_base.yaw + dev::kHeadYawSign * relYaw;
        angles[2] = dev::kHeadRollSign * headRoll;
        float q[4];
        EulerToQuat(angles, q);
        QuatToRot(q, c.rot);
    }

    // Eye position: base position + camera right axis (rotation column 0) * the eye's half-IPD.
    const vr::EyeGeometry& g = vr::Eye(eye);
    const float side = g.offset[0] * dev::kWorldUnitsPerMeter * settings::IpdScale() * dev::kEyeRightSign;
    c.pos[0] = g_base.pos[0] + c.rot[0] * side;
    c.pos[1] = g_base.pos[1] + c.rot[3] * side;
    c.pos[2] = g_base.pos[2] + c.rot[6] * side;

    // Symmetric frustum covering the eye's asymmetric one; Submit bounds crop it back.
    const float tanH = (std::fabs(g.tanLeft) > std::fabs(g.tanRight) ? std::fabs(g.tanLeft) : std::fabs(g.tanRight));
    const float tanV = (std::fabs(g.tanTop) > std::fabs(g.tanBottom) ? std::fabs(g.tanTop) : std::fabs(g.tanBottom));
    c.fullHFov = 2.0f * std::atan(tanH);
    c.aspect = tanH / tanV;
    // OpenVR's raw projection: the image's TOP edge is at tan = tanBottom and its bottom edge at tan = tanTop (y up;
    // from OpenVR's ComposeProjection, and the MOHW mod's "Top/Bottom transposed" note). Texture v = 0 is the top.
    // (First build mapped v from tanTop -> stretch looking up / squash looking down on the asymmetric frustum.)
    c.bounds[0] = (g.tanLeft / tanH + 1.0f) * 0.5f;
    c.bounds[1] = (1.0f - g.tanBottom / tanV) * 0.5f;
    c.bounds[2] = (g.tanRight / tanH + 1.0f) * 0.5f;
    c.bounds[3] = (1.0f - g.tanTop / tanV) * 0.5f;

    // HUD panel: 16:9, HudWidthDeg wide, HudHeightDeg above straight ahead, and centred for each eye on the point
    // HudDistance ahead of the head centre (seen from an eye at x = offset: tan = -offset / distance), so both eyes
    // converge on it there. The D3D hooks squeeze the GUI pass's viewport into it (d3d_probe.cpp). Not the camera's
    // viewport rect (cam+0x3C): the HUD ignores it (2026-10-16, no effect).
    constexpr float kDeg = 0.01745329252f;
    const float hudTanW = std::tan(settings::HudWidthDeg() * 0.5f * kDeg);
    const float hudTanH = hudTanW * (9.0f / 16.0f);
    const float distance = settings::HudDistance() > 0.1f ? settings::HudDistance() : 0.1f;
    const float centreTanX = -g.offset[0] / distance;
    const float centreTanY = std::tan(settings::HudHeightDeg() * kDeg);
    const float centreU = 0.5f + 0.5f * centreTanX / tanH;
    const float centreV = 0.5f - 0.5f * centreTanY / tanV; // v = 0 is the top
    c.hudPanel[0] = hudTanW / tanH;                     // scaleX
    c.hudPanel[1] = hudTanH / tanV;                     // scaleY
    c.hudPanel[2] = centreU - c.hudPanel[0] * 0.5f;     // offsetX
    c.hudPanel[3] = centreV - c.hudPanel[1] * 0.5f;     // offsetY
    return c;
}

void ApplyEye(const EyeCamera& c)
{
    ViewCamera* views = ViewCameras();
    Global<uintptr_t>(addr::g_CurViewCamera) = reinterpret_cast<uintptr_t>(&views[0]);
    Global<int>(addr::g_CurViewIndex) = 0;
    Global<uint8_t>(addr::g_CurViewMask) = 1;
    float pos[3] = {c.pos[0], c.pos[1], c.pos[2]};
    float rot[9];
    std::memcpy(rot, c.rot, sizeof(rot));
    SetRotation(rot);
    SetPosition(pos);
    // The projection builder (FUN_004FA900) takes the horizontal scale from the GLOBAL aspect at 009CCD54, not the
    // camera's +0xE4: P00 = cot(vHalf) / g_aspect, P11 = -cot(vHalf). So the global must hold this eye's aspect while
    // it renders, or the image comes out far wider than the crop assumes (first builds: horizontal squeeze, yaw
    // stretch, roll "morph", and culling at the eye edges because the culling frustum used +0xEC/+0xFC). The FOV
    // setter multiplies its aspect argument by the same global, hence 1.0.
    float& globalAspect = Global<float>(kBaseAspect);
    if (globalAspect != g_lastWrittenAspect)
        g_gameAspect = globalAspect; // the game (re)wrote it: remember its value for the flat frames
    globalAspect = c.aspect;
    g_lastWrittenAspect = c.aspect;
    SetHorizontalFov(c.fullHFov, 1.0f);
    std::memcpy(g_hudPanel, c.hudPanel, sizeof(g_hudPanel));
    Global<uint8_t>(addr::g_CameraDirty) = 1;
}

// Flat frames show the game's own image: give the global aspect back if it still holds ours.
void RestoreGameAspect()
{
    float& globalAspect = Global<float>(kBaseAspect);
    if (g_gameAspect > 0.0f && g_lastWrittenAspect > 0.0f && globalAspect == g_lastWrittenAspect)
    {
        globalAspect = g_gameAspect;
        g_lastWrittenAspect = -1.0f;
    }
}

bool EnsureTextures(ID3D11Device* device, ID3D11Resource* backBuffer)
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(backBuffer->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb)
        return false;
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    bb->Release();
    if (g_eyeTex[0] && d.Width == g_texW && d.Height == g_texH)
        return true;
    for (ID3D11Texture2D** t : {&g_eyeTex[0], &g_eyeTex[1], &g_screenTex})
        if (*t)
        {
            (*t)->Release();
            *t = nullptr;
        }
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = 0;
    d.CPUAccessFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.MipLevels = 1;
    d.ArraySize = 1;
    for (ID3D11Texture2D** t : {&g_eyeTex[0], &g_eyeTex[1], &g_screenTex})
        if (FAILED(device->CreateTexture2D(&d, nullptr, t)))
            return false;
    if (!g_blackTex)
    {
        D3D11_TEXTURE2D_DESC b = d;
        b.Width = b.Height = 16;
        static uint32_t zeros[16 * 16] = {};
        D3D11_SUBRESOURCE_DATA init{zeros, 16 * 4, 0};
        device->CreateTexture2D(&b, &init, &g_blackTex);
    }
    g_texW = d.Width;
    g_texH = d.Height;
    AVP_LOG(kLog, "eye/screen textures %ux%u format %u", g_texW, g_texH, d.Format);
    return true;
}

// Camera apply replacement (shadow vtable slot +0x08 of the player camera component). Main thread, once per frame,
// before the scene render.
void __fastcall CamApplyReplacement(void* self, void* /*edx*/, void* entity)
{
    OriginalCamApply(self, entity);
    vr::TryConnect();
    if (!dev::kVrEnabled || !vr::IsConnected() || g_stereoFrame)
        return;
    // Cutscenes and the in-game (pause) menu go to the flat screen -- with the game's own aspect and HUD viewport
    // already back for this frame's render.
    const bool pausedWithMenu = IsPausedWithMenu();
    LogMenuState(pausedWithMenu);
    if (Global<uint8_t>(addr::g_CinematicActive) || pausedWithMenu)
    {
        RestoreGameAspect();
        return;
    }

    g_head = vr::WaitPoses();
    g_posesWaited = true;
    if (!g_head.valid)
        return;

    std::memcpy(&g_base, &ViewCameras()[0], sizeof(ViewCamera));
    if (g_recenterRequested.exchange(false))
    {
        g_haveRefYaw = false;
        ++g_recenterCount; // head aim keeps the current look heading (vr/head_aim.cpp)
    }
    if (!g_haveRefYaw)
    {
        float y, p, r;
        HeadAngles(g_head, y, p, r);
        g_refYaw = y;
        g_haveRefYaw = true;
        AVP_LOG(kLog, "recentered: head yaw reference %.1f deg", y * 57.29578f);
    }
    ApplyEye(BuildEye(0));
    g_stereoFrame = true;
    g_hudPanelActive = true;
    g_lastStereoTick = GetTickCount();
}

void LogFrame(const EyeCamera& l, const EyeCamera& r)
{
    float hy, hp, hr;
    HeadAngles(g_head, hy, hp, hr);
    const ViewCamera& v = ViewCameras()[0];
    AVP_LOG(kLog,
            "stereo frame %u: head yaw/pitch/roll %.1f/%.1f/%.1f deg, base yaw %.1f pitch %.1f, base pos (%.2f %.2f %.2f), "
            "L pos (%.3f %.3f %.3f) R pos (%.3f %.3f %.3f), cam fov h/v %.1f/%.1f aspect %.3f, bounds L %.3f..%.3f x "
            "%.3f..%.3f, D3D lock recursion %ld (1 = balanced: the game's own, left after Present)",
            g_stereoFrames, hy * 57.3f, hp * 57.3f, hr * 57.3f, g_base.yaw * 57.3f, g_base.pitch * 57.3f, g_base.pos[0],
            g_base.pos[1], g_base.pos[2], l.pos[0], l.pos[1], l.pos[2], r.pos[0], r.pos[1], r.pos[2],
            2 * v.hHalfFov * 57.3f, 2 * v.vHalfFov * 57.3f, v.aspect, l.bounds[0], l.bounds[2], l.bounds[1],
            l.bounds[3], reinterpret_cast<const CRITICAL_SECTION*>(addr::g_D3DLock)->RecursionCount);
}

} // namespace

void Register()
{
    RegisterShadowReplacement(addr::CamComp_ApplyToRenderCamera, reinterpret_cast<void*>(&CamApplyReplacement));
}

const float* HudPanel() { return g_hudPanelActive && settings::HudPanel() ? g_hudPanel : nullptr; }

void RequestRecenter() { g_recenterRequested.store(true); }

bool InEyeRender() { return g_hudPanelActive; }

void EngineEulerMatrix(const float angles[3], float m[9])
{
    float q[4];
    EulerToQuat(angles, q);
    QuatToRot(q, m);
}

bool HeadRotation(float m[9])
{
    if (!g_head.valid || !g_haveRefYaw)
        return false;
    HeadMatrix(g_head, m);
    return true;
}

unsigned RecenterCount() { return g_recenterCount; }

bool HandToWorld(const float pose[3][4], float rot[9], float pos[3])
{
    float frame[9];
    if (!g_head.valid || !g_haveRefYaw || !aim::PlayerFrame(frame))
        return false;
    // Room vector -> recentred room (the reference yaw removed about +y) -> engine local (x, -y, -z: OpenVR -> engine
    // is a 180-degree turn about x) -> world through the player frame (columns right, down, forward).
    const float c = std::cos(-g_refYaw), s = std::sin(-g_refYaw);
    auto toWorld = [&](float vx, float vy, float vz, float out[3]) {
        const float x = vx * c + vz * s, y = vy, z = -vx * s + vz * c;
        const float l[3] = {x, -y, -z};
        for (int i = 0; i < 3; ++i)
            out[i] = frame[i * 3 + 0] * l[0] + frame[i * 3 + 1] * l[1] + frame[i * 3 + 2] * l[2];
    };
    float offset[3];
    toWorld(pose[0][3] - g_head.m[0][3], pose[1][3] - g_head.m[1][3], pose[2][3] - g_head.m[2][3], offset);
    for (int i = 0; i < 3; ++i)
        pos[i] = g_base.pos[i] + offset[i] * dev::kWorldUnitsPerMeter;
    float right[3], down[3], forward[3];
    toWorld(pose[0][0], pose[1][0], pose[2][0], right);      // the controller's +x
    toWorld(-pose[0][1], -pose[1][1], -pose[2][1], down);    // -y
    toWorld(-pose[0][2], -pose[1][2], -pose[2][2], forward); // -z
    for (int i = 0; i < 3; ++i)
    {
        rot[i * 3 + 0] = right[i];
        rot[i * 3 + 1] = down[i];
        rot[i * 3 + 2] = forward[i];
    }
    return true;
}

void GameCamera(float rot[9], float pos[3])
{
    std::memcpy(rot, g_base.rot, sizeof(g_base.rot));
    std::memcpy(pos, g_base.pos, sizeof(g_base.pos));
}

unsigned long PresentThread() { return g_presentThread; }

bool HeadAnglesDeg(float out[3])
{
    if (!g_head.valid)
        return false;
    float yaw, pitch, roll;
    HeadAngles(g_head, yaw, pitch, roll);
    out[0] = (yaw - g_refYaw) * 57.29578f;
    out[1] = pitch * 57.29578f;
    out[2] = roll * 57.29578f;
    return true;
}

bool StereoActive() { return g_lastStereoTick && GetTickCount() - g_lastStereoTick < 500; }

void OnPresent(IDXGISwapChain*, unsigned& syncInterval)
{
    g_presentThread = GetCurrentThreadId();
    vr::TryConnect();
    if (!dev::kVrEnabled || !vr::IsConnected())
        return;
    syncInterval = 0; // WaitGetPoses paces the frame
    if (dev::kDiagRenderEverything)
        Global<uint8_t>(addr::cvar_RenderEverything) = 1;

    auto* device = reinterpret_cast<ID3D11Device*>(Global<uintptr_t>(addr::g_D3DDevice));
    auto* ctx = reinterpret_cast<ID3D11DeviceContext*>(Global<uintptr_t>(addr::g_D3DContext));
    auto* backBuffer = reinterpret_cast<ID3D11Resource*>(Global<uintptr_t>(addr::g_BackBuffer));
    auto* scene = reinterpret_cast<ID3D11Resource*>(Global<uintptr_t>(addr::g_SceneTarget));
    if (!device || !ctx || !backBuffer || !scene || !EnsureTextures(device, backBuffer))
        return;

    if (g_stereoFrame)
    {
        g_stereoFrame = false;
        const EyeCamera left = BuildEye(0), right = BuildEye(1);

        // Left eye = the frame the game just rendered with the left camera.
        ctx->CopyResource(g_eyeTex[0], backBuffer);
        ResetHudViewport(ctx); // the 3D passes must not inherit the squeezed GUI viewport

        // Right eye = a second render (job-batch join skipped: already joined by the first render).
        ApplyEye(right);
        int& kickFrame = Global<int>(addr::g_JobBatchKickFrame);
        const int savedKick = kickFrame;
        if (kickFrame == Global<int>(addr::g_FrameCounter))
            kickFrame = savedKick - 1;
        SetSecondRender(true);
        BeginFrame(1); // enters the D3D lock again (Render_BeginFrame) ...
        RenderScene();
        SetSecondRender(false);
        LeaveCriticalSection(reinterpret_cast<LPCRITICAL_SECTION>(addr::g_D3DLock)); // ... so leave it (addresses.h)
        kickFrame = savedKick;
        ctx->CopyResource(g_eyeTex[1], scene);
        ResetHudViewport(ctx);
        g_hudPanelActive = false;

        if (g_stereoFrames++ < 3 || g_stereoFrames % 600 == 0)
            LogFrame(left, right);

        // Give the game its own camera (and aspect) back until its next camera update: game logic between frames
        // reads view 0. 2026-10-17: with a wide head-centred camera left there instead, the Predator's "look at"
        // objective never completed in VR (whatever turned the view); with the game's camera it does, and no pop-in.
        // (That wide camera was added before the projection fix and head aim, against pop-in at the eye edges that
        // came from those bugs: the culling frustum narrower than the drawing, the game camera pointing elsewhere.)
        // EyeFovBetweenFrames (default): the game's position and orientation, but the eyes' field of view and aspect,
        // so that visibility work covers what the eyes show (2026-10-17: with the game's own ~85 x 54 deg frustum
        // there was some pop-in at the edges).
        std::memcpy(&ViewCameras()[0], &g_base, sizeof(ViewCamera));
        if (settings::EyeFovBetweenFrames())
        {
            EyeCamera wide = BuildEye(0); // its symmetric frustum covers both eyes (the right one mirrors the left)
            std::memcpy(wide.rot, g_base.rot, sizeof(wide.rot));
            std::memcpy(wide.pos, g_base.pos, sizeof(wide.pos));
            ApplyEye(wide);
        }
        else
        {
            RestoreGameAspect();
        }
        Global<uint8_t>(addr::g_CameraDirty) = 1;

        vr::HideScreen();
        vr::SubmitEyes(g_eyeTex[0], left.bounds, g_eyeTex[1], right.bounds);
    }
    else
    {
        // Flat: the game's frame on the virtual screen, black in the eyes.
        g_hudPanelActive = false;
        RestoreGameAspect();
        if (!g_posesWaited)
            g_head = vr::WaitPoses();
        ctx->CopyResource(g_screenTex, backBuffer);
        vr::ShowScreen(g_screenTex, 1.0f, g_head);
        static const float full[4] = {0, 0, 1, 1};
        vr::SubmitEyes(g_blackTex, full, g_blackTex, full);
    }
    g_posesWaited = false;
}

} // namespace avp::stereo
