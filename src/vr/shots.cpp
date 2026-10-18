#include "shots.h"

#include "head_aim.h"
#include "laser.h"
#include "stereo.h"
#include "../core/addresses.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../hooks/first_person.h"
#include "../hooks/vmt_shadow.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace avp::shots {
namespace {

constexpr const char* kLog = "avpvr_shots.log";

constexpr float kLaserLength = 30.0f; // world units (~metres)

// The shots' line on the drawn gun: from the placed weapon model's origin (at the hand) plus the shot origin offsets
// in the gun's frame, along the game's direction carried through the placement plus the shot angle offsets. The
// gun's frame = the game camera's axes through the placement's turn (the drawn gun sits on the controller as the
// game's sits before the camera). m (optional) = the placement (fp::GunPlacement).
bool GunRay(const float gameDir[3], float pos[3], float dir[3], float* mOut = nullptr)
{
    float m[16], origin[3];
    if (!fp::GunPlacement(m, origin))
        return false;
    float camRot[9], camPos[3];
    stereo::GameCamera(camRot, camPos);
    float axis[3][3]; // right, down, forward
    for (int a = 0; a < 3; ++a)
        for (int j = 0; j < 3; ++j)
            axis[a][j] = camRot[0 * 3 + a] * m[0 * 4 + j] + camRot[1 * 3 + a] * m[1 * 4 + j] +
                         camRot[2 * 3 + a] * m[2 * 4 + j];
    float off[3], ang[2];
    settings::ShotOffset(off, ang);
    for (int j = 0; j < 3; ++j)
        pos[j] = origin[j] + off[0] * axis[0][j] - off[1] * axis[1][j] + off[2] * axis[2][j];
    // The direction in the gun's frame (right, down, forward), then pitch (+ up) and yaw (+ right).
    float d[3], l[3];
    for (int j = 0; j < 3; ++j)
        d[j] = gameDir[0] * m[0 * 4 + j] + gameDir[1] * m[1 * 4 + j] + gameDir[2] * m[2 * 4 + j];
    for (int a = 0; a < 3; ++a)
        l[a] = d[0] * axis[a][0] + d[1] * axis[a][1] + d[2] * axis[a][2];
    constexpr float kRad = 0.01745329252f;
    const float cp = std::cos(ang[0] * kRad), sp = std::sin(ang[0] * kRad);
    const float cy = std::cos(ang[1] * kRad), sy = std::sin(ang[1] * kRad);
    const float y1 = l[1] * cp - l[2] * sp, z1 = l[1] * sp + l[2] * cp;
    const float x2 = l[0] * cy + z1 * sy, z2 = -l[0] * sy + z1 * cy;
    for (int j = 0; j < 3; ++j)
        d[j] = x2 * axis[0][j] + y1 * axis[1][j] + z2 * axis[2][j];
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-6f)
        return false;
    for (int j = 0; j < 3; ++j)
        dir[j] = d[j] / len;
    if (mOut)
        std::memcpy(mOut, m, sizeof(m));
    return true;
}

bool IsLocalMarine(void* character)
{
    return character && character == aim::LocalCharacter() && Global<int>(addr::g_PlayerSpecies) == 2;
}

// The fire transform, moved onto the gun on the controller (see shots.h). pos[3], dir[3]; mat = the muzzle's 3x3
// (columns = its axes; the type-6 weapon fires along column 2), turned the same way for weapons that read it.
void FromGun(void* weapon, void* character, float* pos, float* dir, float* mat)
{
    if (!settings::ShotsFromGun() || !IsLocalMarine(character))
        return;
    static DWORD lastLog = 0;
    static int logged = 0;
    const DWORD now = GetTickCount();
    const bool log = logged < 8 || now - lastLog > 2000;
    const float p0[3] = {pos[0], pos[1], pos[2]}, d0[3] = {dir[0], dir[1], dir[2]};
    float m[16];
    if (!GunRay(d0, pos, dir, m))
    {
        if (log)
        {
            lastLog = now, ++logged;
            AVP_LOG(kLog, "shot: no weapon placement (no first-person weapon drawn lately, or no controller pose)");
        }
        return;
    }
    // The muzzle's axes (columns) through the same turn: new column = old column * R (as row vectors).
    if (mat)
    {
        float out[9];
        for (int col = 0; col < 3; ++col)
            for (int j = 0; j < 3; ++j)
                out[j * 3 + col] = mat[0 * 3 + col] * m[0 * 4 + j] + mat[1 * 3 + col] * m[1 * 4 + j] +
                                   mat[2 * 3 + col] * m[2 * 4 + j];
        std::memcpy(mat, out, sizeof(out));
    }
    if (log)
    {
        lastLog = now, ++logged;
        // The game's direction in its camera's frame (right, down, forward): how it sits relative to the view.
        float camRot[9], camPos[3];
        stereo::GameCamera(camRot, camPos);
        float local[3];
        for (int k = 0; k < 3; ++k)
            local[k] = camRot[0 * 3 + k] * d0[0] + camRot[1 * 3 + k] * d0[1] + camRot[2 * 3 + k] * d0[2];
        uint32_t vtable = 0, type = 0;
        std::memcpy(&vtable, weapon, 4);
        std::memcpy(&type, static_cast<const char*>(weapon) + 0x40, 4);
        AVP_LOG(kLog, "shot: weapon %p (type %u, vtable shadow %08X): game pos (%.2f %.2f %.2f) dir (%.3f %.3f %.3f) "
                "[camera frame r/d/f %.3f %.3f %.3f] -> gun pos (%.2f %.2f %.2f) dir (%.3f %.3f %.3f)", weapon, type,
                vtable, p0[0], p0[1], p0[2], d0[0], d0[1], d0[2], local[0], local[1], local[2], pos[0], pos[1], pos[2],
                dir[0], dir[1], dir[2]);
    }
}

// thiscall(weapon; character, muzzleHash, ammo value, range, outPos, outDir, outMat, outDist, outTarget), RET 0x24.
// The two floats pass through as raw bits.
using FireTransformFn = void(__fastcall*)(void* weapon, void* edx, void* character, uint32_t muzzleHash,
                                          uint32_t ammoValue, uint32_t range, float* outPos, float* outDir,
                                          float* outMat, float* outDist, int* outTarget);

template <uintptr_t Original>
void __fastcall FireTransformReplacement(void* weapon, void* edx, void* character, uint32_t muzzleHash,
                                         uint32_t ammoValue, uint32_t range, float* outPos, float* outDir,
                                         float* outMat, float* outDist, int* outTarget)
{
    reinterpret_cast<FireTransformFn>(Original)(weapon, edx, character, muzzleHash, ammoValue, range, outPos, outDir,
                                                outMat, outDist, outTarget);
    if (outPos && outDir)
        FromGun(weapon, character, outPos, outDir, outMat);
}

} // namespace

void OnLook(void* character, const float look[3])
{
    float start[3], dir[3], ts[3] = {}, te[3] = {}, head[3] = {};
    bool visible = false;
    if (settings::LaserSight() && settings::ShotsFromGun() && IsLocalMarine(character) && GunRay(look, start, dir))
    {
        const float end[3] = {start[0] + dir[0] * kLaserLength, start[1] + dir[1] * kLaserLength,
                              start[2] + dir[2] * kLaserLength};
        visible = stereo::WorldToTracking(start, ts, head) && stereo::WorldToTracking(end, te);
    }
    laser::Update(visible, ts, te, head);
}

void Register()
{
    RegisterShadowReplacement(addr::Weapon_GetFireTransform,
                              reinterpret_cast<void*>(&FireTransformReplacement<addr::Weapon_GetFireTransform>));
    RegisterShadowReplacement(addr::Weapon_GetFireTransformCached,
                              reinterpret_cast<void*>(&FireTransformReplacement<addr::Weapon_GetFireTransformCached>));
    RegisterShadowReplacement(addr::Weapon_GetFireTransformMuzzle,
                              reinterpret_cast<void*>(&FireTransformReplacement<addr::Weapon_GetFireTransformMuzzle>));
    AVP_LOG(kLog, "shots from the gun: GetFireTransform replacements registered (3 originals, 14 weapon classes)");
}

} // namespace avp::shots
