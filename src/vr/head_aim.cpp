#include "head_aim.h"

#include "stereo.h"
#include "vr_input.h"
#include "../core/addresses.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../hooks/vmt_shadow.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <mutex>

namespace avp::aim {
namespace {

constexpr const char* kLog = "avpvr_aim.log";
constexpr float kDeg = 57.29578f;
constexpr float kStickDeadzone = 0.15f;

using PFN_SetLookDirection = void(__thiscall*)(void* ctrl, void* character, float* dir, char aimRaycast);
using PFN_BodyMatrix = const float*(__thiscall*)(void* bodyObject);

const auto OriginalSetLookDirection = reinterpret_cast<PFN_SetLookDirection>(addr::LookCtrl_SetLookDirection);

struct V3
{
    float x, y, z;
};
V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Scale(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Normalize(V3& a)
{
    const float len = std::sqrt(Dot(a, a));
    if (len > 1e-6f)
        a = Scale(a, 1.0f / len);
    return len;
}

// Rodrigues: v rotated about the unit axis k by angle (radians).
V3 Rotate(V3 v, V3 k, float angle)
{
    const float c = std::cos(angle), s = std::sin(angle);
    return Add(Add(Scale(v, c), Scale(Cross(k, v), s)), Scale(k, Dot(k, v) * (1.0f - c)));
}

// The character's body frame, as UpdateLook gets it (006AD476): the virtual base subobject at
// character + 4 + vbtable[2] (vbptr at character+4), its vtable +0x0C, a zero-argument thiscall returning 9 floats
// (row-major; columns = the body's right, up, forward).
bool BodyMatrix(void* character, float m[9])
{
    __try
    {
        const auto c = reinterpret_cast<uintptr_t>(character);
        const uintptr_t vbtable = *reinterpret_cast<const uintptr_t*>(c + 4);
        const int offset = *reinterpret_cast<const int*>(vbtable + 8);
        void* body = reinterpret_cast<void*>(c + 4 + offset);
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(body);
        const auto fn = *reinterpret_cast<const PFN_BodyMatrix*>(vt + 0x0C);
        const float* p = fn(body);
        if (!p)
            return false;
        std::memcpy(m, p, 9 * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SafeReadFloats(uintptr_t a, float* out, int n)
{
    __try
    {
        std::memcpy(out, reinterpret_cast<const void*>(a), n * sizeof(float));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Whether ctrl is the local player's look controller (multiplayer: other players' characters can have controllers
// of the same classes, and the shadow scanner hooks those too -- they must stay untouched). Every controller keeps its
// owner's entity handle at +0x04 (006AB140 / LookCtrl_Ctor); the local player's handle is at [g_GameScene] + 8
// (FUN_0081A800). (First try, EntityRegistry_Find(handle) + 0x30, found nothing for the Predator.)
bool IsLocalPlayerController(void* ctrl, int* ownerOut, int* playerOut)
{
    __try
    {
        const uintptr_t scene = Global<uintptr_t>(addr::g_GameScene);
        const int player = scene ? *reinterpret_cast<const int*>(scene + 8) : -1;
        const int owner = *reinterpret_cast<const int*>(reinterpret_cast<uintptr_t>(ctrl) + 4);
        *ownerOut = owner;
        *playerOut = player;
        return scene && owner == player;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
void MatMul(const float a[9], const float b[9], float c[9])
{
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            c[r * 3 + k] = a[r * 3 + 0] * b[0 * 3 + k] + a[r * 3 + 1] * b[1 * 3 + k] + a[r * 3 + 2] * b[2 * 3 + k];
}

// ---- State: the look updates' thread writes, the eye cameras read the frame (mutex) ----
bool g_active = false;
void* g_ctrl = nullptr;
V3 g_up{0, 1, 0};
V3 g_heading{0, 0, 1};
V3 g_lastLook{0, 0, 1};
unsigned g_recenterSeen = 0;
DWORD g_thread = 0;

std::mutex g_frameMutex;
float g_frame[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
float g_lookFrame[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
DWORD g_frameTick = 0;
bool g_frameValid = false;

void Deactivate(const char* why)
{
    if (!g_active)
        return;
    g_active = false;
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        g_frameValid = false;
    }
    AVP_LOG(kLog, "head aim off: %s", why);
}

// The heading projected onto the plane perpendicular to up (fallback when it's (nearly) parallel to up).
V3 Flatten(V3 v, V3 up, V3 fallback)
{
    V3 h = Sub(v, Scale(up, Dot(v, up)));
    if (Normalize(h) < 0.05f)
    {
        h = Sub(fallback, Scale(up, Dot(fallback, up)));
        Normalize(h);
    }
    return h;
}

// Returns true with out = the head-driven look; false = leave the game's own.
bool Steer(void* ctrl, void* character, const float* gameDir, float out[4])
{
    if (!settings::HeadAim())
    {
        Deactivate("setting off");
        return false;
    }
    if (!stereo::StereoActive())
    {
        Deactivate("not in VR gameplay");
        return false;
    }
    int owner = 0, player = 0;
    if (!IsLocalPlayerController(ctrl, &owner, &player))
    {
        static void* lastOther = nullptr;
        if (ctrl != lastOther)
        {
            lastOther = ctrl;
            AVP_LOG(kLog, "controller %p (owner handle %08X) isn't the local player's (handle %08X): left alone", ctrl,
                    owner, player);
        }
        return false;
    }
    float head[9], m[9];
    if (!stereo::HeadRotation(head) || !BodyMatrix(character, m))
        return false;
    // The body's vertical axis (matrix column 1; the engine's y points down, so this is the body's DOWN direction --
    // the frame below keeps the engine's column convention: right, down, forward). Named "up" for the axis it turns about.
    V3 up{m[1], m[4], m[7]};
    if (Normalize(up) < 0.5f)
        return false;
    const V3 bodyForward{m[2], m[5], m[8]};
    V3 game{gameDir[0], gameDir[1], gameDir[2]};
    Normalize(game);

    if (!g_active || ctrl != g_ctrl)
    {
        // Start from the game's look: the heading is its direction along the body's plane. Recentering makes the
        // head's current yaw "straight ahead", so the view starts where the game was looking.
        g_heading = Flatten(game, up, bodyForward);
        g_up = up;
        g_lastLook = game;
        g_ctrl = ctrl;
        g_active = true;
        g_thread = GetCurrentThreadId();
        stereo::RequestRecenter();
        g_recenterSeen = stereo::RecenterCount() + 1; // the requested one
        int speciesNow = -1;
        SafeReadFloats(addr::g_PlayerSpecies, reinterpret_cast<float*>(&speciesNow), 1);
        uintptr_t characterVt = 0;
        SafeReadFloats(reinterpret_cast<uintptr_t>(character), reinterpret_cast<float*>(&characterVt), 1);
        float slots[2] = {};
        if (characterVt)
            SafeReadFloats(characterVt + 0xB0, slots, 2); // get look / set look, for the netcode trace
        uint32_t getLook = 0, setLook = 0;
        std::memcpy(&getLook, &slots[0], 4);
        std::memcpy(&setLook, &slots[1], 4);
        AVP_LOG(kLog, "head aim on: controller %p, character %p (vtable %08X: +B0 %08X, +B4 %08X), species %d, thread "
                      "%lu (Present thread %lu), heading (%.2f %.2f %.2f), up (%.2f %.2f %.2f)",
                ctrl, character, static_cast<unsigned>(characterVt), getLook, setLook, speciesNow, g_thread,
                stereo::PresentThread(), g_heading.x, g_heading.y, g_heading.z, up.x, up.y, up.z);
    }
    else
    {
        // Carry the heading along when the up axis turns (Alien walls / ceilings): the minimal rotation g_up -> up.
        V3 axis = Cross(g_up, up);
        const float s = Normalize(axis), c = Dot(g_up, up);
        if (s > 1e-5f)
            g_heading = Rotate(g_heading, axis, std::atan2(s, c));
        g_up = up;
        g_heading = Flatten(g_heading, up, bodyForward);
    }

    // A recenter keeps the view: the heading becomes the last look's (the head yaw reference just reset to 0).
    const unsigned recenters = stereo::RecenterCount();
    if (recenters != g_recenterSeen)
    {
        g_recenterSeen = recenters;
        g_heading = Flatten(g_lastLook, up, g_heading);
    }

    // Smooth turn: the right stick turns the heading about up (+ x = right).
    float dt = Global<float>(addr::g_FrameDt);
    dt = dt < 0.0f ? 0.0f : dt > 0.1f ? 0.1f : dt;
    input::PadState pad{};
    if (input::GetPad(&pad) && std::fabs(pad.turnX) > kStickDeadzone)
    {
        const float x = (std::fabs(pad.turnX) - kStickDeadzone) / (1.0f - kStickDeadzone) * (pad.turnX > 0 ? 1.0f : -1.0f);
        const float angle = x * settings::TurnSpeedDeg() / kDeg * dt;
        const V3 right = Cross(up, g_heading);
        g_heading = Add(Scale(g_heading, std::cos(angle)), Scale(right, std::sin(angle)));
    }

    // Orthonormal player frame (columns right, up, forward) and the look = its forward times the head.
    V3 right = Cross(up, g_heading);
    Normalize(right);
    g_heading = Cross(right, up);
    const float frame[9] = {right.x, up.x, g_heading.x, right.y, up.y, g_heading.y, right.z, up.z, g_heading.z};
    float view[9];
    MatMul(frame, head, view);
    // Per-class aim offset (settings AimOffset*): the look turned up / right relative to the view, in the view's own
    // frame, for species whose shots land off the view's centre (2026-10-17: the Marine's aim sat slightly above).
    int species = -1;
    SafeReadFloats(addr::g_PlayerSpecies, reinterpret_cast<float*>(&species), 1);
    float offUp = 0.0f, offRight = 0.0f;
    settings::AimOffsetDeg(species, &offUp, &offRight);
    if (offUp != 0.0f || offRight != 0.0f)
    {
        const float angles[3] = {offUp / kDeg, offRight / kDeg, 0.0f}; // engine angles: + pitch = up, + yaw = right
        float offset[9], aimed[9];
        stereo::EngineEulerMatrix(angles, offset);
        MatMul(view, offset, aimed);
        std::memcpy(view, aimed, sizeof(view));
    }
    V3 look{view[2], view[5], view[8]};
    Normalize(look);

    // The controller's own pitch limits (up 90 deg - ctrl+0xD0, down 90 deg - ctrl+0xD4, relative to the body), so the
    // look saturates where the game's would; the view goes on with the head. "up" here is the body matrix's column 1,
    // which points DOWN (the engine's y axis does): dot > 0 = looking down.
    float limits[2] = {0.087f, 0.262f};
    SafeReadFloats(reinterpret_cast<uintptr_t>(ctrl) + 0xD0, limits, 2);
    const float maxDownDot = std::cos(limits[1]), maxUpDot = -std::cos(limits[0]);
    const float sinPitch = Dot(look, up);
    bool clamped = false;
    if (sinPitch > maxDownDot || sinPitch < maxUpDot)
    {
        const float sc = sinPitch > maxDownDot ? maxDownDot : maxUpDot;
        const V3 horizontal = Flatten(look, up, g_heading);
        look = Add(Scale(horizontal, std::sqrt(1.0f - sc * sc)), Scale(up, sc));
        clamped = true;
    }
    g_lastLook = look;
    out[0] = look.x;
    out[1] = look.y;
    out[2] = look.z;
    out[3] = 0.0f;

    // The look's own frame without roll (forward = the look, up from the player frame): what the game's camera is
    // built from this frame, so the eye cameras can take the game camera's difference from it (LookFrame).
    V3 lookRight = Cross(up, look);
    if (Normalize(lookRight) < 1e-3f)
        lookRight = right;
    const V3 lookUp = Cross(look, lookRight);
    const float lookFrame[9] = {lookRight.x, lookUp.x, look.x, lookRight.y, lookUp.y, look.y,
                                lookRight.z, lookUp.z, look.z};
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        std::memcpy(g_frame, frame, sizeof(g_frame));
        std::memcpy(g_lookFrame, lookFrame, sizeof(g_lookFrame));
        g_frameTick = GetTickCount();
        g_frameValid = true;
    }

    static DWORD lastLog = 0;
    const DWORD now = GetTickCount();
    if (now - lastLog >= 1000)
    {
        lastLog = now;
        AVP_LOG(kLog,
                "look (%.3f %.3f %.3f)%s | game (%.3f %.3f %.3f) | heading (%.3f %.3f %.3f) up (%.3f %.3f %.3f) | stick "
                "x %.2f dt %.4f",
                look.x, look.y, look.z, clamped ? " clamped" : "", game.x, game.y, game.z, g_heading.x, g_heading.y,
                g_heading.z, up.x, up.y, up.z, pad.turnX, dt);
    }
    return true;
}

// Shadow-vtable replacement for slot +0x9C (thiscall(ctrl, character, dir, aimRaycast), RET 0xC).
void __fastcall SetLookDirectionReplacement(void* ctrl, void* /*edx*/, void* character, float* dir, char aimRaycast)
{
    // 16-byte aligned: SetLookDirection reads it with MOVAPS (006AC212); a plain stack array crashed the first
    // steered call (2026-10-17, access violation at FFFFFFFF = misaligned SSE load).
    alignas(16) float out[4];
    if (Steer(ctrl, character, dir, out))
        OriginalSetLookDirection(ctrl, character, out, aimRaycast);
    else
        OriginalSetLookDirection(ctrl, character, dir, aimRaycast);
}

} // namespace

void Register()
{
    RegisterShadowReplacement(addr::LookCtrl_SetLookDirection, reinterpret_cast<void*>(&SetLookDirectionReplacement));
}

bool PlayerFrame(float m[9], float look[9])
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    if (!g_frameValid || GetTickCount() - g_frameTick > 250)
        return false;
    std::memcpy(m, g_frame, sizeof(g_frame));
    if (look)
        std::memcpy(look, g_lookFrame, sizeof(g_lookFrame));
    return true;
}

} // namespace avp::aim
