#include "stereo.h"

#include "head_aim.h"
#include "opaque_pass.h"
#include "openvr_session.h"
#include "vr_input.h"
#include "../core/addresses.h"
#include "../core/dev_flags.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../hooks/d3d_probe.h"
#include "../hooks/video_hook.h"
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
float g_refHeadPos[3] = {};   // head position then (tracking space): head position's zero
unsigned g_stereoFrames = 0;
float g_gameAspect = 0.0f;         // the game's global aspect (009CCD54) before VR changed it
float g_lastWrittenAspect = -1.0f; // what VR last wrote there
float g_lastWrittenVHalfFov = -1.0f; // view 0's vertical half FOV as VR last wrote it
float g_gameProjection[10] = {};       // view 0's +0xE4..+0x10C as the game itself last left it
bool g_haveGameProjection = false;

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

// A tracking-space vector in the game world, as hands and head movement are mapped: the reference yaw removed about
// +y, OpenVR -> engine local (x, -y, -z: a 180-degree turn about x), through the player frame (columns right, down,
// forward).
void TrackingToWorld(const float frame[9], float vx, float vy, float vz, float out[3])
{
    const float c = std::cos(-g_refYaw), s = std::sin(-g_refYaw);
    const float x = vx * c + vz * s, y = vy, z = -vx * s + vz * c;
    const float l[3] = {x, -y, -z};
    for (int i = 0; i < 3; ++i)
        out[i] = frame[i * 3 + 0] * l[0] + frame[i * 3 + 1] * l[1] + frame[i * 3 + 2] * l[2];
}

// ---- Head position (settings HeadPosition, RoomScaleStep*) ----
// The play space is anchored in the world: the real head is at headWorld = anchor + map(head - recentred head), map =
// TrackingToWorld. The residual r = headWorld - the game camera (the character's eye) is split along the body's up:
// - Across (walking, leaning): the eyes follow the head. With RoomScaleStep, once the head is RoomScaleStepDistance
//   off, the character STEPS under it -- left-stick input (input::SetMoveAssist) while the player's own stick is idle,
//   so the game's own collision and movement (and multiplayer, which replicates the input) apply -- and stops there;
//   it steps again only once the head is that far off again. The view runs ahead of the character by at most the step
//   distance + kLeadMargin (inside its collision capsule, so it can't see through walls); past that + kSlideMargin
//   (blocked by a wall, knocked back) the anchor slides: the play space moves rather than the character being pulled
//   back. The player's own stick moves the anchor with the character (artificial locomotion carries the play space),
//   and so does the coast after it, until the character has stopped. Without RoomScaleStep: view only, as in the MOHW
//   mod (the anchor always travels with the character; up to kMaxViewOnly).
// - Up and down (crouching, standing): view only, up to kMaxVertical.
// Turning (the player frame turns: stick turn, Alien surfaces) pivots the play space about the head, so it doesn't
// start a step. The hands are measured from the eye (g_eyeOffset), so they stay with the real head.
//
// 2026-10-17's first room-scale walk circled the spot after the player's own stick input, worse with a faster step:
// the anchor stopped travelling the moment the stick was let go, the character's coast then counted as residual and
// started a walk back (a 10 cm start), which overshot and started again. Now the coast is carried, a step only starts
// past the step distance (30 cm default), stops once it arrives (aimed at where the coast will end), and an overshoot
// stays as view offset instead of starting another walk; a step that makes no progress (a wall) gives up.
constexpr float kLeadMargin = 0.10f;  // metres the view may lead the character past the step distance
constexpr float kSlideMargin = 0.15f; // ... and past that the play space slides
constexpr float kMaxViewOnly = 1.5f;  // metres across, view only
constexpr float kMaxVertical = 0.6f;  // metres up / down
constexpr float kFollowStop = 0.04f;  // a step is over once it arrives inside this
constexpr float kFollowGain = 1.0f;   // stick per metre of residual, on top of RoomScaleStepSpeed
constexpr float kCoastSeconds = 0.25f;     // how far ahead the step aims: the character coasts after the stick lets go
constexpr float kStoppedSpeed = 0.2f;      // m/s: the character has stopped (the coast after the player's stick is over)
constexpr float kVelocitySmoothing = 0.3f; // per frame
constexpr float kUserStickActive = 0.15f;
constexpr float kTeleport = 2.0f;           // metres in one frame: respawn / teleport, re-anchor
constexpr DWORD kStepNoProgressMs = 600;    // a step whose residual hasn't shrunk by kStepProgress for this long ...
constexpr float kStepProgress = 0.02f;      // ... gives up (blocked) until the head comes back within the step distance

float g_eyeOffset[3] = {}; // this frame: the eyes' offset from the game camera (world)
float g_anchor[3] = {};
bool g_haveAnchor = false;
unsigned g_anchorRecenter = 0;
float g_lastFrame[9] = {};
float g_lastBase[3] = {};
bool g_following = false;  // a step is under way
bool g_coasting = false;   // the player's stick was let go, the character is still moving: the anchor still travels
bool g_stepBlocked = false; // the last step gave up: none until the head is back within the step distance
float g_stepBest = 0.0f;   // the step's smallest residual so far (m) ...
DWORD g_stepBestTick = 0;  // ... and when
float g_velocity[3] = {};  // the character's speed across (world units / s, smoothed)

void HeadPositionOff()
{
    g_eyeOffset[0] = g_eyeOffset[1] = g_eyeOffset[2] = 0.0f;
    g_haveAnchor = false;
    g_following = g_coasting = g_stepBlocked = false;
    g_velocity[0] = g_velocity[1] = g_velocity[2] = 0.0f;
    input::SetMoveAssist(0.0f, 0.0f);
}

// Once per stereo frame, after the poses and the game camera (g_base) are in.
void UpdateHeadPosition()
{
    float frame[9];
    if (!settings::HeadPosition() || !g_haveRefYaw || !aim::PlayerFrame(frame))
    {
        HeadPositionOff();
        return;
    }
    const bool roomScale = settings::RoomScaleStep();
    const float stepAt = settings::RoomScaleStepDistance();
    const float rel[3] = {g_head.m[0][3] - g_refHeadPos[0], g_head.m[1][3] - g_refHeadPos[1],
                          g_head.m[2][3] - g_refHeadPos[2]};
    float mapped[3];
    TrackingToWorld(frame, rel[0], rel[1], rel[2], mapped);
    for (float& v : mapped)
        v *= dev::kWorldUnitsPerMeter;
    const float* base = g_base.pos;
    input::PadState pad{};
    const bool userMoving =
        input::GetPad(&pad) && std::sqrt(pad.moveX * pad.moveX + pad.moveY * pad.moveY) > kUserStickActive;
    const float down[3] = {frame[1], frame[4], frame[7]}; // the body's down
    float moved[3] = {base[0] - g_lastBase[0], base[1] - g_lastBase[1], base[2] - g_lastBase[2]};
    const float movedLen = std::sqrt(moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2]);
    // The player's own locomotion and its coast: until the character has stopped (last frame's speed).
    const float speedBefore = std::sqrt(g_velocity[0] * g_velocity[0] + g_velocity[1] * g_velocity[1] +
                                        g_velocity[2] * g_velocity[2]) / dev::kWorldUnitsPerMeter;
    if (userMoving)
        g_coasting = true;
    else if (g_coasting && speedBefore < kStoppedSpeed)
        g_coasting = false;
    if (g_haveAnchor && movedLen > kTeleport * dev::kWorldUnitsPerMeter)
    {
        AVP_LOG(kLog, "head position: the character jumped %.1f m in a frame (respawn, teleport): re-anchored",
                movedLen / dev::kWorldUnitsPerMeter);
        g_haveAnchor = false;
    }
    if (!g_haveAnchor || g_anchorRecenter != g_recenterCount)
    {
        for (int i = 0; i < 3; ++i)
            g_anchor[i] = base[i] - mapped[i]; // the head on the eye
        g_haveAnchor = true;
        g_anchorRecenter = g_recenterCount;
        g_following = g_stepBlocked = false;
    }
    else
    {
        // Turning: keep the head where it is in the world.
        float mappedOld[3];
        TrackingToWorld(g_lastFrame, rel[0], rel[1], rel[2], mappedOld);
        for (int i = 0; i < 3; ++i)
            g_anchor[i] += (mappedOld[i] - mapped[i]) * dev::kWorldUnitsPerMeter;
        // The character's movement up and down (jumps, stairs, lifts) always carries the play space; across, only the
        // player's own locomotion and its coast do (or everything, view only) -- the step after the head mustn't.
        const float dv = moved[0] * down[0] + moved[1] * down[1] + moved[2] * down[2];
        const bool across = !roomScale || g_coasting;
        for (int i = 0; i < 3; ++i)
            g_anchor[i] += across ? moved[i] : down[i] * dv;
    }
    std::memcpy(g_lastFrame, frame, sizeof(frame));
    std::memcpy(g_lastBase, base, sizeof(g_lastBase));

    float r[3];
    for (int i = 0; i < 3; ++i)
        r[i] = g_anchor[i] + mapped[i] - base[i];
    const float v = r[0] * down[0] + r[1] * down[1] + r[2] * down[2];
    float h[3] = {r[0] - down[0] * v, r[1] - down[1] * v, r[2] - down[2] * v};
    float len = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    const float leadM = stepAt + kLeadMargin;
    const float maxAcross = (roomScale ? leadM + kSlideMargin : kMaxViewOnly) * dev::kWorldUnitsPerMeter;
    if (len > maxAcross)
    {
        const float k = maxAcross / len;
        for (int i = 0; i < 3; ++i)
        {
            g_anchor[i] -= h[i] * (1.0f - k); // the play space slides
            h[i] *= k;
        }
        len = maxAcross;
    }

    // The character's speed across (smoothed), for the step's braking.
    LARGE_INTEGER qpc, freq;
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&freq);
    static LONGLONG lastQpc = 0;
    const float dt = lastQpc ? static_cast<float>(qpc.QuadPart - lastQpc) / static_cast<float>(freq.QuadPart) : 0.0f;
    lastQpc = qpc.QuadPart;
    {
        const float dv = moved[0] * down[0] + moved[1] * down[1] + moved[2] * down[2];
        for (int i = 0; i < 3; ++i)
        {
            const float vi = dt > 0.001f && dt < 0.1f ? (moved[i] - down[i] * dv) / dt : 0.0f;
            g_velocity[i] += (vi - g_velocity[i]) * kVelocitySmoothing;
        }
    }
    const float speed = std::sqrt(g_velocity[0] * g_velocity[0] + g_velocity[1] * g_velocity[1] +
                                  g_velocity[2] * g_velocity[2]) / dev::kWorldUnitsPerMeter;

    // The step after the head, aimed where the head will be once the character has coasted (kCoastSeconds at its
    // speed): that releases the stick in time to stop on it, and steers out sideways drift. Stick axes = the game
    // camera's right / forward made level (the move stick goes where the view looks).
    float ax = 0.0f, ay = 0.0f;
    const float lenM = len / dev::kWorldUnitsPerMeter;
    const DWORD now = GetTickCount();
    if (lenM < stepAt)
        g_stepBlocked = false; // the head came back: steps may start again
    if (roomScale && !userMoving && !g_coasting)
    {
        if (!g_following && !g_stepBlocked && lenM > stepAt)
        {
            g_following = true;
            g_stepBest = lenM;
            g_stepBestTick = now;
        }
        float pred[3];
        for (int i = 0; i < 3; ++i)
            pred[i] = h[i] - g_velocity[i] * kCoastSeconds;
        const float predLen = std::sqrt(pred[0] * pred[0] + pred[1] * pred[1] + pred[2] * pred[2]);
        const float predM = predLen / dev::kWorldUnitsPerMeter;
        const bool arriving = predM < kFollowStop || pred[0] * h[0] + pred[1] * h[1] + pred[2] * h[2] <= 0.0f;
        if (g_following)
        {
            if (lenM < g_stepBest - kStepProgress)
            {
                g_stepBest = lenM;
                g_stepBestTick = now;
            }
            if (arriving)
                g_following = false; // arrived: what's left stays as view offset
            else if (now - g_stepBestTick > kStepNoProgressMs)
            {
                g_following = false;
                g_stepBlocked = true;
                AVP_LOG(kLog, "head position: the step made no progress (blocked) at %.2f m: stopped", lenM);
            }
        }
        if (g_following && predLen > 1e-6f)
        {
            float right[3] = {g_base.rot[0], g_base.rot[3], g_base.rot[6]};
            float fwd[3] = {g_base.rot[2], g_base.rot[5], g_base.rot[8]};
            const float dr = right[0] * down[0] + right[1] * down[1] + right[2] * down[2];
            const float df = fwd[0] * down[0] + fwd[1] * down[1] + fwd[2] * down[2];
            for (int i = 0; i < 3; ++i)
            {
                right[i] -= down[i] * dr;
                fwd[i] -= down[i] * df;
            }
            const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
            const float fl = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
            if (rl > 1e-4f && fl > 1e-4f)
            {
                float m = settings::RoomScaleStepSpeed() + (predM - kFollowStop) * kFollowGain;
                m = m > 1.0f ? 1.0f : m;
                ax = (pred[0] * right[0] + pred[1] * right[1] + pred[2] * right[2]) / (rl * predLen) * m;
                ay = (pred[0] * fwd[0] + pred[1] * fwd[1] + pred[2] * fwd[2]) / (fl * predLen) * m;
            }
        }
    }
    else
        g_following = false;
    input::SetMoveAssist(ax, ay);

    // The eyes: across, ahead of the character by at most the lead (room-scale) or the whole residual (view only);
    // up and down as the head.
    const float lead = roomScale ? leadM * dev::kWorldUnitsPerMeter : maxAcross;
    const float k = len > lead && len > 1e-6f ? lead / len : 1.0f;
    const float vMax = kMaxVertical * dev::kWorldUnitsPerMeter;
    const float vc = v > vMax ? vMax : v < -vMax ? -vMax : v;
    for (int i = 0; i < 3; ++i)
        g_eyeOffset[i] = h[i] * k + down[i] * vc;

    static DWORD lastLog = 0;
    static bool wasFollowing = false;
    if (now - lastLog > 2000 || g_following != wasFollowing)
    {
        lastLog = now;
        wasFollowing = g_following;
        AVP_LOG(kLog,
                "head position: residual across %.2f m, up/down %.2f m, speed %.2f m/s, %s, stick assist (%.2f %.2f)%s",
                lenM, -v / dev::kWorldUnitsPerMeter, speed,
                !roomScale        ? "view only"
                : g_following     ? "stepping under the head"
                : g_stepBlocked   ? "room-scale (last step blocked)"
                : g_coasting      ? "room-scale (coasting after the stick)"
                                  : "room-scale",
                ax, ay, userMoving ? ", player's stick" : "");
    }
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

    // Eye position: base position + the head's movement (head position) + camera right axis (rotation column 0) * the
    // eye's half-IPD.
    const vr::EyeGeometry& g = vr::Eye(eye);
    const float side = g.offset[0] * dev::kWorldUnitsPerMeter * settings::IpdScale() * dev::kEyeRightSign;
    c.pos[0] = g_base.pos[0] + g_eyeOffset[0] + c.rot[0] * side;
    c.pos[1] = g_base.pos[1] + g_eyeOffset[1] + c.rot[3] * side;
    c.pos[2] = g_base.pos[2] + g_eyeOffset[2] + c.rot[6] * side;

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
    const float distance = settings::HudDistance() > 0.01f ? settings::HudDistance() : 0.01f;
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
    g_lastWrittenVHalfFov = views[0].vHalfFov; // RestoreGameView: still ours?
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

// Flat frames (the in-game menu, cinematics, movies) after gameplay: the global aspect, and view 0's own projection
// (aspect and FOV, +0xE4..+0x10C) if it still holds what the mod last wrote -- EyeFovBetweenFrames leaves the eyes' wide
// field of view there between frames, and the game doesn't rewrite it for those screens, which came out cut off top
// and bottom, movies partly black (2026-10-18). A cinematic camera that set its own is left alone.
void RestoreGameView()
{
    RestoreGameAspect();
    ViewCamera& v = ViewCameras()[0];
    if (g_lastWrittenVHalfFov > 0.0f && v.vHalfFov == g_lastWrittenVHalfFov && g_haveGameProjection)
    {
        std::memcpy(&v.aspect, g_gameProjection, sizeof(g_gameProjection));
        Global<uint8_t>(addr::g_CameraDirty) = 1;
        AVP_LOG(kLog, "flat frame: view 0's projection given back (v half FOV %.1f -> %.1f deg)",
                g_lastWrittenVHalfFov * 57.29578f, v.vHalfFov * 57.29578f);
    }
    g_lastWrittenVHalfFov = -1.0f;
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
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; // render target: opaque_pass.h
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
    {
        // Which camera component applies (2026-10-18: an Alien sequence after an X press -- no movie, pause or
        // cinematic flag -- rendered into the HUD panel's box; is it another camera?). Its shadow vtable names the
        // class (avpvr_shadow.log). Each change logged, at most 20 a second.
        static uintptr_t lastVt = 0;
        static void* lastSelf = nullptr;
        static DWORD second = 0;
        static int inSecond = 0;
        const uintptr_t vt = *static_cast<const uintptr_t*>(self);
        if (vt != lastVt || self != lastSelf)
        {
            const DWORD now = GetTickCount();
            if (now - second >= 1000)
                second = now, inSecond = 0;
            if (++inSecond <= 20)
                AVP_LOG(kLog, "camera apply: component %p (shadow vtable %08X), entity %p; cinematic %d, menu screen %d",
                        self, static_cast<unsigned>(vt), entity, Global<uint8_t>(addr::g_CinematicActive),
                        IsMenuScreenUp());
            lastVt = vt;
            lastSelf = self;
        }
    }
    {
        // The game's own projection, whenever view 0 holds one the mod didn't write (RestoreGameView).
        const ViewCamera& v = ViewCameras()[0];
        if (v.vHalfFov > 0.0f && v.vHalfFov != g_lastWrittenVHalfFov)
        {
            std::memcpy(g_gameProjection, &v.aspect, sizeof(g_gameProjection));
            g_haveGameProjection = true;
        }
    }
    vr::TryConnect();
    if (!dev::kVrEnabled || !vr::IsConnected() || g_stereoFrame)
        return;
    // Cutscenes and the in-game menus go to the flat screen -- with the game's own view back for this frame's render.
    // A menu screen up counts too: the multiplayer menu doesn't pause the game (2026-10-18: it rendered in the eyes,
    // cut off; "paused with menu" only catches the single player one).
    const bool pausedWithMenu = IsPausedWithMenu();
    LogMenuState(pausedWithMenu);
    const bool cinematic = Global<uint8_t>(addr::g_CinematicActive) != 0;
    static bool wasFlat = false;
    // And movies: some play during gameplay with neither (hooks/video_hook.h).
    const bool movie = video::MoviePlaying();
    if (cinematic || pausedWithMenu || IsMenuScreenUp() || movie)
    {
        RestoreGameView();
        if (!wasFlat)
        {
            // What the game renders the flat frame with (movies came out black but for a cloak effect, 2026-10-18).
            const ViewCamera* views = ViewCameras();
            const uintptr_t cur = Global<uintptr_t>(addr::g_CurViewCamera);
            AVP_LOG(kLog,
                    "flat frame starts: cinematic %d, movie %d, paused menu %d, menu screen %d; current view %d (camera %08X = view "
                    "%d), view mask %02X, views active %d%d%d%d, view 0 v half FOV %.1f deg, global aspect %.3f",
                    cinematic, movie, pausedWithMenu, IsMenuScreenUp(), Global<int>(addr::g_CurViewIndex),
                    static_cast<unsigned>(cur), static_cast<int>((cur - reinterpret_cast<uintptr_t>(views)) / sizeof(ViewCamera)),
                    Global<uint8_t>(addr::g_CurViewMask), views[0].active, views[1].active, views[2].active, views[3].active,
                    views[0].vHalfFov * 57.29578f, Global<float>(kBaseAspect));
        }
        wasFlat = true;
        return;
    }
    wasFlat = false;

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
        for (int i = 0; i < 3; ++i)
            g_refHeadPos[i] = g_head.m[i][3];
        g_haveRefYaw = true;
        AVP_LOG(kLog, "recentered: head yaw reference %.1f deg, head position reference (%.2f %.2f %.2f) m",
                y * 57.29578f, g_refHeadPos[0], g_refHeadPos[1], g_refHeadPos[2]);
    }
    UpdateHeadPosition();
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
    // The hand's offset from the head, and its axes, through TrackingToWorld; from the eyes (the game camera + the
    // head position's eye offset).
    auto toWorld = [&](float vx, float vy, float vz, float out[3]) { TrackingToWorld(frame, vx, vy, vz, out); };
    float offset[3];
    toWorld(pose[0][3] - g_head.m[0][3], pose[1][3] - g_head.m[1][3], pose[2][3] - g_head.m[2][3], offset);
    for (int i = 0; i < 3; ++i)
        pos[i] = g_base.pos[i] + g_eyeOffset[i] + offset[i] * dev::kWorldUnitsPerMeter;
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

bool WorldToTracking(const float world[3], float tracking[3], float head[3])
{
    float frame[9];
    if (!g_head.valid || !g_haveRefYaw || !aim::PlayerFrame(frame))
        return false;
    // HandToWorld backwards: world offset from the eyes -> engine local (frame^T) -> OpenVR (x, -y, -z) -> the
    // reference yaw put back -> plus the head.
    float o[3], l[3];
    for (int i = 0; i < 3; ++i)
        o[i] = (world[i] - g_base.pos[i] - g_eyeOffset[i]) / dev::kWorldUnitsPerMeter;
    for (int k = 0; k < 3; ++k)
        l[k] = frame[0 * 3 + k] * o[0] + frame[1 * 3 + k] * o[1] + frame[2 * 3 + k] * o[2];
    const float x = l[0], y = -l[1], z = -l[2];
    const float c = std::cos(-g_refYaw), s = std::sin(-g_refYaw);
    tracking[0] = x * c - z * s + g_head.m[0][3];
    tracking[1] = y + g_head.m[1][3];
    tracking[2] = x * s + z * c + g_head.m[2][3];
    if (head)
        for (int i = 0; i < 3; ++i)
            head[i] = g_head.m[i][3];
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
        RestoreGameView();
        if (!g_posesWaited)
            g_head = vr::WaitPoses();
        ctx->CopyResource(g_screenTex, backBuffer);
        opaque::MakeOpaque(device, ctx, g_screenTex); // the back buffer's alpha is 0 under the 3D scene and movies
        vr::ShowScreen(g_screenTex, 1.0f, g_head);
        static const float full[4] = {0, 0, 1, 1};
        vr::SubmitEyes(g_blackTex, full, g_blackTex, full);
    }
    g_posesWaited = false;
}

} // namespace avp::stereo
