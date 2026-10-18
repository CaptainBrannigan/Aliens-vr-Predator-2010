#include "first_person.h"

#include "hud_passes.h"
#include "../core/addresses.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../vr/stereo.h"
#include "../vr/vr_input.h"

#include <MinHook.h>
#include <windows.h>
#include <d3d11.h>
#include <intrin.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace avp::fp {
namespace {

constexpr const char* kLog = "avpvr_fp.log";
constexpr uint32_t kBodyPassReturn = 0x00808E14; // Scene_RenderFrame's call of 0053AB20 (the mesh queue)
constexpr UINT kBoneBufferBytes = 6288;
constexpr UINT kBonesOffset = 0x90;
constexpr int kMaxBones = 128;
constexpr float kFirstPersonRadius = 1.5f; // world units (~metres) from the game camera
constexpr unsigned kMaxProbeDraw = 40;

constexpr int kCtxMap = 14;
constexpr int kCtxUnmap = 15;
constexpr int kCtxUpdateSubresource = 48;

using PFN_Map = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                            D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PFN_UpdateSubresource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                                       const void*, UINT, UINT);
PFN_Map g_realMap = nullptr;
PFN_Unmap g_realUnmap = nullptr;
PFN_UpdateSubresource g_realUpdateSubresource = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
int g_bodyPass = -1;

// ---- The bone buffer and its latest game upload (render thread only) ----
ID3D11Resource* g_boneBuffer = nullptr; // compared only (the game's own, lives for the session)
void* g_mapped = nullptr;
alignas(16) float g_snap[kBoneBufferBytes / 4];
unsigned g_fillSeq = 0, g_handledFill = 0;

HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags,
                                   D3D11_MAPPED_SUBRESOURCE* out)
{
    const HRESULT hr = g_realMap(c, r, sub, type, flags, out);
    if (SUCCEEDED(hr) && out && c == g_ctx && r && r == g_boneBuffer)
        g_mapped = out->pData;
    return hr;
}

void STDMETHODCALLTYPE Hook_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub)
{
    if (c == g_ctx && r && r == g_boneBuffer && g_mapped)
    {
        std::memcpy(g_snap, g_mapped, kBoneBufferBytes);
        g_mapped = nullptr;
        ++g_fillSeq;
    }
    g_realUnmap(c, r, sub);
}

void STDMETHODCALLTYPE Hook_UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box,
                                              const void* src, UINT rowPitch, UINT depthPitch)
{
    if (c == g_ctx && r && r == g_boneBuffer && src && !box)
    {
        std::memcpy(g_snap, src, kBoneBufferBytes);
        ++g_fillSeq;
    }
    g_realUpdateSubresource(c, r, sub, box, src, rowPitch, depthPitch);
}

void HookEntry(void* object, int index, void* detour, void** original, const char* name)
{
    void* target = (*reinterpret_cast<void***>(object))[index];
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK)
        s = MH_EnableHook(target);
    AVP_LOG(kLog, "hook %s at %p: %s", name, target, MH_StatusToString(s));
}

// ---- Models ----
enum class Role
{
    Other,
    Body,
    Weapon,
};
const char* RoleName(Role r) { return r == Role::Body ? "BODY" : r == Role::Weapon ? "weapon" : "other"; }

Role g_role = Role::Other;          // the model being drawn
bool g_modelFirstPerson = false;    // it's within reach of the camera
ID3D11Buffer* g_modelVB = nullptr;  // its vertex buffer (identity)
unsigned g_modelDraws = 0;          // its parts drawn so far
ID3D11Buffer* g_bodyVB = nullptr;   // the body's vertex buffer (chosen per frame)

// First-person models seen this frame: vertex buffer -> parts (draws).
struct FpModel
{
    ID3D11Buffer* vb;
    unsigned draws;
};
FpModel g_frameModels[8];
int g_frameModelCount = 0;

void NoteFrameModel(ID3D11Buffer* vb, unsigned draws)
{
    for (int i = 0; i < g_frameModelCount; ++i)
        if (g_frameModels[i].vb == vb)
        {
            if (draws > g_frameModels[i].draws)
                g_frameModels[i].draws = draws;
            return;
        }
    if (g_frameModelCount < 8)
        g_frameModels[g_frameModelCount++] = {vb, draws};
}

// The body's parts this frame (game render), for the bone map.
struct Part
{
    unsigned start, count;
    int base;
};
std::vector<Part> g_bodyParts;
ID3D11Buffer* g_bodyIB = nullptr;
UINT g_bodyStride = 0;
DXGI_FORMAT g_bodyIndexFormat = DXGI_FORMAT_R16_UINT;
ID3D11Buffer* g_mappedBodyVB = nullptr; // the body whose bone map was logged
// From the bone map: each bone's weighted bind-space centroid (its vertices' centre) and total weight.
float g_boneCentroid[kMaxBones][3] = {};
float g_boneWeight[kMaxBones] = {};
bool g_haveCentroids = false;
bool g_armLogged[2] = {}; // the arm move's first result per hand, logged once per bone map

// The Marine body's arms (bone map 2026-10-17, 0-based; FINDINGS section 9): upper arm + its twist bone, forearm + its
// twist bone (at the wrist), hand + fingers. Everything else (clavicle, shoulder, torso) stays where the game puts it.
struct ArmRig
{
    int upper, upperTwist, fore, foreTwist, handFirst, handLast;
};
constexpr ArmRig kMarineArms[2] = {{7, 26, 8, 25, 9, 24}, {29, 48, 30, 47, 31, 46}};
// The Marine's upper body: above the pelvis (1) the stomach 2, spine 3, chest 4, head 5, clavicles / arms / shoulders
// 6-49, top 50, front pouches 51-52 (belly height); the legs 53-60 and the belt 61 stay with the pelvis.
constexpr int kMarineStomach = 2, kMarineChest = 4, kMarineUpperLast = 52;
enum ArmSegment
{
    kSegOther,
    kSegUpper,
    kSegFore,
    kSegHand,
};
int SegmentOf(const ArmRig& r, int bone)
{
    if (bone == r.upper || bone == r.upperTwist)
        return kSegUpper;
    if (bone == r.fore || bone == r.foreTwist)
        return kSegFore;
    if (bone >= r.handFirst && bone <= r.handLast)
        return kSegHand;
    return kSegOther;
}
// From the bone map: each arm's shoulder, elbow and wrist in bind space (the centre of the vertices skinned across
// the joint's two segments).
float g_armJoint[2][3][3] = {};
bool g_haveJoints[2] = {};

// ---- Matrices (row-major 4x4, row vectors: v' = v * M; rows 0-2 = axes, row 3 = translation) ----
void Mul4(const float a[16], const float b[16], float c[16])
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            c[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
}

// Inverse of an affine row-vector matrix (3x3 part general, last column 0 0 0 1).
bool InverseAffine(const float m[16], float out[16])
{
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
    const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-8f)
        return false;
    const float id = 1.0f / det;
    const float r[9] = {(e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id,
                        (f * g - d * k) * id, (a * k - c * g) * id, (c * d - a * f) * id,
                        (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id};
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
            out[i * 4 + j] = r[i * 3 + j];
        out[i * 4 + 3] = 0.0f;
    }
    const float t[3] = {m[12], m[13], m[14]};
    for (int j = 0; j < 3; ++j)
        out[12 + j] = -(t[0] * r[0 * 3 + j] + t[1] * r[1 * 3 + j] + t[2] * r[2 * 3 + j]);
    out[15] = 1.0f;
    return true;
}

// ---- Bones in the buffer: each = the source's row-major 4x4 S (v' = v * S) stored as its columns 0-2 (3 float4),
// with S[2][2] replaced by the per-bone factor (the shader rebuilds it; FINDINGS section 9) ----
float* BoneSlot(float* buffer, int bone) { return buffer + kBonesOffset / 4 + bone * 12; }

void ReadBone(const float* b, float s[16], float* factor)
{
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 4; ++row)
            s[row * 4 + col] = b[col * 4 + row];
    s[3] = s[7] = s[11] = 0.0f;
    s[15] = 1.0f;
    *factor = b[10];
    s[10] = s[0] * s[5] - s[1] * s[4]; // rotation row 2 = row 0 x row 1: its z
}

void WriteBone(float* b, const float s[16], float factor)
{
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 4; ++row)
            b[col * 4 + row] = s[row * 4 + col];
    b[10] = factor;
}

// The controller's frame with the angle offsets (engine angles in the controller's own frame: + pitch up, + yaw right,
// + roll) and its point with the position offsets (right, up, forward, metres).
void ApplyOffsets(float rot[9], float pos[3], const float off[3], const float anglesDeg[3])
{
    constexpr float kRad = 0.01745329252f;
    for (int j = 0; j < 3; ++j)
        pos[j] += rot[j * 3 + 0] * off[0] - rot[j * 3 + 1] * off[1] + rot[j * 3 + 2] * off[2];
    if (anglesDeg[0] == 0.0f && anglesDeg[1] == 0.0f && anglesDeg[2] == 0.0f)
        return;
    const float a[3] = {anglesDeg[0] * kRad, anglesDeg[1] * kRad, anglesDeg[2] * kRad};
    float e[9], r[9];
    stereo::EngineEulerMatrix(a, e);
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            r[i * 3 + k] = rot[i * 3 + 0] * e[0 * 3 + k] + rot[i * 3 + 1] * e[1 * 3 + k] + rot[i * 3 + 2] * e[2 * 3 + k];
    std::memcpy(rot, r, sizeof(r));
}

// The controller's pose (hand 0 left, 1 right) in the world, offsets applied.
bool ControllerWorld(int hand, const float off[3], const float angles[3], float rot[9], float pos[3])
{
    float pose[3][4];
    if (!input::GetHandPose(hand, true, pose) || !stereo::HandToWorld(pose, rot, pos))
        return false;
    ApplyOffsets(rot, pos, off, angles);
    return true;
}

// ---- Small vector helpers (float3; points through row-vector 4x4s) ----
void Sub3(const float a[3], const float b[3], float o[3])
{
    for (int k = 0; k < 3; ++k)
        o[k] = a[k] - b[k];
}
float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float Len3(const float a[3]) { return std::sqrt(Dot3(a, a)); }
void Cross3(const float a[3], const float b[3], float o[3])
{
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
bool Normalize3(float a[3])
{
    const float l = Len3(a);
    if (l < 1e-6f)
        return false;
    for (int k = 0; k < 3; ++k)
        a[k] /= l;
    return true;
}
// v minus its part along the unit axis, normalised.
bool PerpTo(const float axis[3], const float v[3], float o[3])
{
    const float dn = Dot3(v, axis);
    for (int k = 0; k < 3; ++k)
        o[k] = v[k] - dn * axis[k];
    return Normalize3(o);
}
void PointTimes(const float p[3], const float m[16], float o[3])
{
    float r[3];
    for (int j = 0; j < 3; ++j)
        r[j] = p[0] * m[0 * 4 + j] + p[1] * m[1 * 4 + j] + p[2] * m[2 * 4 + j] + m[3 * 4 + j];
    std::memcpy(o, r, sizeof(r));
}
void VecTimes(const float v[3], const float r[9], float o[3]) // v * R (row vector, 3x3)
{
    float t[3];
    for (int j = 0; j < 3; ++j)
        t[j] = v[0] * r[0 * 3 + j] + v[1] * r[1 * 3 + j] + v[2] * r[2 * 3 + j];
    std::memcpy(o, t, sizeof(t));
}
// The rotation (row vectors) taking the orthonormal frame (x, z x x, z) onto (x2, z2 x x2, z2): A^T * A2.
void FrameToFrame(const float x[3], const float z[3], const float x2[3], const float z2[3], float r[9])
{
    float y[3], y2[3];
    Cross3(z, x, y);
    Cross3(z2, x2, y2);
    const float* a[3] = {x, y, z};
    const float* b[3] = {x2, y2, z2};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = a[0][i] * b[0][j] + a[1][i] * b[1][j] + a[2][i] * b[2][j];
}
// Rotation (row vectors) by angle about the unit axis, right-handed.
void AxisRotation(const float a[3], float angle, float r[9])
{
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    const float col[9] = {c + a[0] * a[0] * t,        a[0] * a[1] * t - a[2] * s, a[0] * a[2] * t + a[1] * s,
                          a[1] * a[0] * t + a[2] * s, c + a[1] * a[1] * t,        a[1] * a[2] * t - a[0] * s,
                          a[2] * a[0] * t - a[1] * s, a[2] * a[1] * t + a[0] * s, c + a[2] * a[2] * t};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = col[j * 3 + i];
}
// The model-space move v' = (v - from) * R + to.
void PivotMove(const float r[9], const float from[3], const float to[3], float m[16])
{
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j)
            m[i * 4 + j] = r[i * 3 + j];
        m[i * 4 + 3] = 0.0f;
    }
    float f[3];
    VecTimes(from, r, f);
    for (int j = 0; j < 3; ++j)
        m[12 + j] = to[j] - f[j];
    m[15] = 1.0f;
}
void MoveBone(float* buffer, int bone, const float m[16])
{
    float s[16], factor, ns[16];
    float* slot = BoneSlot(buffer, bone);
    ReadBone(slot, s, &factor);
    Mul4(s, m, ns);
    WriteBone(slot, ns, factor);
}

// The body model's level forward and up in the world. Its frame (Marine, 2026-10-17): model y = down, model z =
// sideways (a "forward" offset along z moved the body sideways), so model x is forward or back -- which one is settled
// once per body against the view's direction (the body faces roughly where the player looks).
float g_bodyForwardSign = 0.0f; // 0 = not settled yet
bool BodyAxes(const float w[16], float fwd[3], float up[3])
{
    float fx[3] = {w[0], w[1], w[2]}, uy[3] = {-w[4], -w[5], -w[6]};
    if (!Normalize3(fx) || !Normalize3(uy))
        return false;
    if (g_bodyForwardSign == 0.0f)
    {
        float camRot[9], camPos[3];
        stereo::GameCamera(camRot, camPos);
        const float d = fx[0] * camRot[2] + fx[1] * camRot[5] + fx[2] * camRot[8]; // . the view's forward (column 2)
        if (std::fabs(d) < 0.5f)
            return false;
        g_bodyForwardSign = d > 0.0f ? 1.0f : -1.0f;
        AVP_LOG(kLog, "body model: forward = model %sx (view direction . model x = %.2f)", d > 0.0f ? "+" : "-", d);
    }
    for (int k = 0; k < 3; ++k)
    {
        fwd[k] = fx[k] * g_bodyForwardSign;
        up[k] = uy[k];
    }
    return true;
}

// The share of the hand's roll (about the forearm) that the forearm twist bone at the wrist takes.
constexpr float kForeTwistShare = 0.5f;

// One arm onto its controller. The hand (bones handFirst..handLast) moves rigidly so its centre -- the hand bones'
// bind-space centroids under their current matrices -- sits on the controller, turned so it keeps the angle to the
// controller that it had to the game camera. Then two-bone IK: the shoulder stays, the upper arm and forearm turn
// about the shoulder and elbow so the wrist meets the hand; the elbow goes where the forearm would continue straight
// back from the hand (the game's wrist angle), on the circle the two lengths allow. Past full reach the arm is straight
// and the shoulder follows the hand. The forearm twist bone takes part of the hand's roll. Without joints from the
// bone map the whole arm moves rigidly with the hand.
bool ArmToController(float* buffer, int hand, const ArmRig& r)
{
    if (!g_haveCentroids)
        return false;
    float off[3], angles[3];
    settings::HandOffset(hand, off, angles);
    float ctrlRot[9], ctrlPos[3];
    if (!ControllerWorld(hand, off, angles, ctrlRot, ctrlPos))
    {
        if (!g_armLogged[hand])
            AVP_LOG(kLog, "arms: no %s controller pose yet", hand ? "right" : "left");
        g_armLogged[hand] = true;
        return false;
    }
    float camRot[9], camPos[3];
    stereo::GameCamera(camRot, camPos);
    float w[16], winv[16];
    std::memcpy(w, buffer, sizeof(w));
    if (!InverseAffine(w, winv))
        return false;
    // The game's matrices of the segments (before any move).
    float su[16], sf[16], sh[16], factor;
    ReadBone(BoneSlot(buffer, r.upper), su, &factor);
    ReadBone(BoneSlot(buffer, r.fore), sf, &factor);
    ReadBone(BoneSlot(buffer, r.handFirst), sh, &factor);
    // The hand's centre now, model space then world.
    float p[3] = {}, total = 0.0f;
    for (int b = r.handFirst; b <= r.handLast; ++b)
    {
        const float wt = g_boneWeight[b];
        if (wt <= 0.0f)
            continue;
        float s[16], c[3];
        ReadBone(BoneSlot(buffer, b), s, &factor);
        PointTimes(g_boneCentroid[b], s, c);
        for (int k = 0; k < 3; ++k)
            p[k] += wt * c[k];
        total += wt;
    }
    if (total <= 0.0f)
        return false;
    for (float& v : p)
        v /= total;
    float pw[3];
    PointTimes(p, w, pw);
    // The hand's world move (row vectors): v' = (v - pw) * Rd + ctrlPos, Rd = camRot * ctrlRot^T (camera frame ->
    // controller frame); in model space W * T * W^-1, applied after each bone's own matrix.
    float t[16] = {};
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            t[i * 4 + k] = camRot[i * 3 + 0] * ctrlRot[k * 3 + 0] + camRot[i * 3 + 1] * ctrlRot[k * 3 + 1] +
                           camRot[i * 3 + 2] * ctrlRot[k * 3 + 2];
    for (int k = 0; k < 3; ++k)
        t[12 + k] = ctrlPos[k] - (pw[0] * t[0 * 4 + k] + pw[1] * t[1 * 4 + k] + pw[2] * t[2 * 4 + k]);
    t[15] = 1.0f;
    float wt4[16], tm[16];
    Mul4(w, t, wt4);
    Mul4(wt4, winv, tm);
    for (int b = r.handFirst; b <= r.handLast; ++b)
        MoveBone(buffer, b, tm);

    const bool firstLog = !g_armLogged[hand];
    g_armLogged[hand] = true;
    if (firstLog)
    {
        const float dx = pw[0] - camPos[0], dy = pw[1] - camPos[1], dz = pw[2] - camPos[2];
        AVP_LOG(kLog, "arms: %s hand (bones %d-%d) at (%.2f %.2f %.2f), %.2f m from the camera; moved to the controller "
                "at (%.2f %.2f %.2f)", hand ? "right" : "left", r.handFirst + 1, r.handLast + 1, pw[0], pw[1], pw[2],
                std::sqrt(dx * dx + dy * dy + dz * dz), ctrlPos[0], ctrlPos[1], ctrlPos[2]);
    }
    auto rigid = [&](const char* why) {
        for (int b : {r.upper, r.upperTwist, r.fore, r.foreTwist})
            MoveBone(buffer, b, tm);
        if (firstLog)
            AVP_LOG(kLog, "arms: %s arm moved rigidly (%s)", hand ? "right" : "left", why);
        return true;
    };
    if (!g_haveJoints[hand])
        return rigid("no joints from the bone map");

    // The joints now (model space), each segment's own: the upper arm from the shoulder to its elbow end, the forearm
    // from its elbow to its wrist end, the hand's wrist. Each segment turns about its own root, so the ends meet
    // wherever the bone map's joint estimates sit.
    const float(*jt)[3] = g_armJoint[hand];
    float shoulder[3], elbowU[3], elbow[3], wristF[3], wrist[3];
    PointTimes(jt[0], su, shoulder);
    PointTimes(jt[1], su, elbowU);
    PointTimes(jt[1], sf, elbow);
    PointTimes(jt[2], sf, wristF);
    PointTimes(jt[2], sh, wrist);
    float upperV[3], foreV[3];
    Sub3(elbowU, shoulder, upperV);
    Sub3(wristF, elbow, foreV);
    const float lu = Len3(upperV), lf = Len3(foreV);
    if (lu < 1e-3f || lf < 1e-3f)
        return rigid("degenerate joints");

    // The target: the wrist carried with the hand; the hand's rotation.
    float wrist2[3], handRot[9];
    PointTimes(wrist, tm, wrist2);
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            handRot[i * 3 + k] = tm[i * 4 + k];

    // Reach: shoulder -> target, clamped to what the two lengths allow (the shoulder follows past full reach).
    float shoulder2[3], n[3];
    std::memcpy(shoulder2, shoulder, sizeof(shoulder2));
    Sub3(wrist2, shoulder, n);
    const float wanted = Len3(n);
    if (!Normalize3(n))
        return rigid("hand on the shoulder");
    const float reachMax = (lu + lf) * 0.999f, reachMin = std::fabs(lu - lf) * 1.001f + 1e-3f;
    float dist = wanted;
    if (dist > reachMax || dist < reachMin)
    {
        dist = dist > reachMax ? reachMax : reachMin;
        for (int k = 0; k < 3; ++k)
            shoulder2[k] = wrist2[k] - n[k] * dist;
    }
    const float x = (lu * lu - lf * lf + dist * dist) / (2.0f * dist);
    const float radius = std::sqrt(std::fmax(0.0f, lu * lu - x * x));

    // The elbow's side: toward where the forearm would continue straight back from the hand.
    const float foreDir[3] = {foreV[0] / lf, foreV[1] / lf, foreV[2] / lf};
    const float down[3] = {0.0f, 1.0f, 0.0f}; // model y points down
    float foreDir2[3], hint[3], side[3];
    VecTimes(foreDir, handRot, foreDir2);
    for (int k = 0; k < 3; ++k)
        hint[k] = wrist2[k] - lf * foreDir2[k] - shoulder2[k];
    if (!PerpTo(n, hint, side) && !PerpTo(n, upperV, side) && !PerpTo(n, down, side))
        return rigid("no elbow direction");
    // Elbow swivel (ElbowSwivelDeg): the elbow turned about the shoulder-wrist line, + = the sense that takes the arm's
    // outward side toward down (outward = the chest bone's bind-space side of this arm, through its current matrix).
    if (const float swivel = settings::ElbowSwivelDeg())
    {
        float chest[16], outP[3], downP[3], cr[3];
        ReadBone(BoneSlot(buffer, kMarineChest), chest, &factor);
        const float sign = hand ? 1.0f : -1.0f; // bind x: left arm -, right arm +
        const float out[3] = {sign * chest[0], sign * chest[1], sign * chest[2]};
        if (PerpTo(n, out, outP) && PerpTo(n, down, downP))
        {
            Cross3(outP, downP, cr);
            float rs[9];
            AxisRotation(n, swivel * 0.01745329252f * (Dot3(cr, n) >= 0.0f ? 1.0f : -1.0f), rs);
            VecTimes(side, rs, side);
        }
    }
    float elbow2[3];
    for (int k = 0; k < 3; ++k)
        elbow2[k] = shoulder2[k] + n[k] * x + side[k] * radius;

    // Hinge frames: axis along each segment, the bend normal (upper x fore), old and new.
    float ux[3] = {upperV[0] / lu, upperV[1] / lu, upperV[2] / lu}, ux2[3], fx2[3], bend[3], bend2[3];
    Sub3(elbow2, shoulder2, ux2);
    Sub3(wrist2, elbow2, fx2);
    if (!Normalize3(ux2) || !Normalize3(fx2))
        return rigid("degenerate solve");
    Cross3(upperV, foreV, bend);
    if (!Normalize3(bend))
    {
        Cross3(ux, down, bend); // the game's arm is straight: any plane through it
        if (!Normalize3(bend))
            return rigid("straight arm along the vertical");
    }
    Cross3(ux2, fx2, bend2);
    if (!Normalize3(bend2))
        Cross3(side, n, bend2); // straight after the solve: the plane of the elbow's side
    float zu[3], zf[3], zu2[3], zf2[3];
    if (!PerpTo(ux, bend, zu) || !PerpTo(foreDir, bend, zf) || !PerpTo(ux2, bend2, zu2) || !PerpTo(fx2, bend2, zf2))
        return rigid("degenerate frames");
    float ru[9], rf[9], mu[16], mf[16];
    FrameToFrame(ux, zu, ux2, zu2, ru);
    FrameToFrame(foreDir, zf, fx2, zf2, rf);
    PivotMove(ru, shoulder, shoulder2, mu);
    PivotMove(rf, elbow, elbow2, mf);
    MoveBone(buffer, r.upper, mu);
    MoveBone(buffer, r.upperTwist, mu);
    MoveBone(buffer, r.fore, mf);

    // Forearm twist: the hand's roll about the new forearm (measured on the forearm's bend normal), shared.
    float zfHand[3], proj[3], cr[3];
    VecTimes(zf, handRot, zfHand);
    float roll = 0.0f;
    if (PerpTo(fx2, zfHand, proj))
    {
        Cross3(zf2, proj, cr);
        roll = std::atan2(Dot3(cr, fx2), Dot3(zf2, proj));
        float rt[9], tw[16], mt[16];
        AxisRotation(fx2, roll * kForeTwistShare, rt);
        PivotMove(rt, elbow2, elbow2, tw);
        Mul4(mf, tw, mt);
        MoveBone(buffer, r.foreTwist, mt);
    }
    else
        MoveBone(buffer, r.foreTwist, mf);
    if (firstLog)
    {
        // Where the model's shoulder sits from the view, along the body's forward / up and sideways, for the body
        // offset settings.
        float sw[3], rel[3], fwd[3], up[3], lateral[3] = {w[8], w[9], w[10]};
        PointTimes(shoulder, w, sw);
        Sub3(sw, camPos, rel);
        const bool axes = BodyAxes(w, fwd, up) && Normalize3(lateral);
        AVP_LOG(kLog, "arms: %s arm IK: upper arm %.3f, forearm %.3f, shoulder to hand %.3f (%s), hand roll on the "
                "forearm %.0f deg; shoulder from the view: forward %.2f, up %.2f, sideways %.2f m%s",
                hand ? "right" : "left", lu, lf, wanted,
                wanted > reachMax ? "past full reach: the shoulder follows" : "within reach", roll * 57.29578f,
                axes ? Dot3(rel, fwd) : 0.0f, axes ? Dot3(rel, up) : 0.0f, axes ? std::fabs(Dot3(rel, lateral)) : 0.0f,
                axes ? "" : " (body forward not settled yet)");
    }
    return true;
}

// The weapon's world matrix moved onto the right controller: its axes and pose relative to the game camera carried
// over to the controller's frame, its origin at the controller + the offsets (controller frame: right, up, forward).
bool PlaceOnController(float* header)
{
    float off[3], angles[3];
    settings::GunOffset(off, angles);
    float ctrlRot[9], ctrlPos[3];
    if (!ControllerWorld(1, off, angles, ctrlRot, ctrlPos))
        return false;
    float camRot[9], camPos[3];
    stereo::GameCamera(camRot, camPos);
    float w[16], wvp[16], winv[16], vp[16];
    std::memcpy(w, header, sizeof(w));
    std::memcpy(wvp, header + 16, sizeof(wvp));
    if (!InverseAffine(w, winv))
        return false;
    Mul4(winv, wvp, vp); // the view * projection the game used

    float nw[16] = {};
    for (int i = 0; i < 3; ++i)
    {
        // The model axis in the camera's frame (camera columns = right, down, forward), then in the controller's.
        float local[3];
        for (int k = 0; k < 3; ++k)
            local[k] = camRot[0 * 3 + k] * w[i * 4 + 0] + camRot[1 * 3 + k] * w[i * 4 + 1] + camRot[2 * 3 + k] * w[i * 4 + 2];
        for (int j = 0; j < 3; ++j)
            nw[i * 4 + j] = ctrlRot[j * 3 + 0] * local[0] + ctrlRot[j * 3 + 1] * local[1] + ctrlRot[j * 3 + 2] * local[2];
        nw[i * 4 + 3] = 0.0f;
    }
    for (int j = 0; j < 3; ++j)
        nw[12 + j] = ctrlPos[j]; // the offsets are in ctrlPos / ctrlRot already
    nw[15] = 1.0f;
    float nwvp[16];
    Mul4(nw, vp, nwvp);
    std::memcpy(header, nw, sizeof(nw));
    std::memcpy(header + 16, nwvp, sizeof(nwvp));
    return true;
}

// The body model moved from the view along its own level forward and up (BodyOffset, BodyAxes), world and WVP
// rewritten as for the weapon. Render only: the view, the look and the shots are untouched.
bool OffsetBody(float* header)
{
    float forward, up;
    settings::BodyOffset(&forward, &up);
    if (forward == 0.0f && up == 0.0f)
        return false;
    float w[16], wvp[16], winv[16], vp[16], fwdAxis[3], upAxis[3];
    std::memcpy(w, header, sizeof(w));
    std::memcpy(wvp, header + 16, sizeof(wvp));
    if (!InverseAffine(w, winv) || !BodyAxes(w, fwdAxis, upAxis))
        return false;
    Mul4(winv, wvp, vp);
    for (int k = 0; k < 3; ++k)
        w[12 + k] += forward * fwdAxis[k] + up * upAxis[k];
    Mul4(w, vp, wvp);
    std::memcpy(header, w, sizeof(w));
    std::memcpy(header + 16, wvp, sizeof(wvp));
    return true;
}

bool IsMarineHandBone(int b)
{
    for (const ArmRig& r : kMarineArms)
        if (b >= r.handFirst && b <= r.handLast)
            return true;
    return false;
}

// Torso turn (TorsoTurnDeg): the upper body from the stomach up turned about the body's vertical through the spine at
// the stomach (its centroid's height and depth, centred sideways), + = the body's forward toward the view's right.
// The hands are left out: their placement on the controllers starts from the game's own pose, and the arm IK then
// joins the turned shoulders to them.
bool TurnTorso(float* buffer)
{
    const int first = kMarineStomach;
    const float degrees = settings::TorsoTurnDeg();
    if (!g_haveCentroids || degrees == 0.0f || g_boneWeight[first] <= 0.0f)
        return false;
    float w[16], winv[16], fwd[3], up[3];
    std::memcpy(w, buffer, sizeof(w));
    if (!InverseAffine(w, winv) || !BodyAxes(w, fwd, up))
        return false;
    float camRot[9], camPos[3], side[3];
    stereo::GameCamera(camRot, camPos);
    Cross3(up, fwd, side); // where a positive turn about up takes the forward
    const float camRight[3] = {camRot[0], camRot[3], camRot[6]};
    const float angle = degrees * 0.01745329252f * (Dot3(side, camRight) >= 0.0f ? 1.0f : -1.0f);
    // The pivot: on the spine at the bone's centroid height (bind x = 0 is the body's middle), through its matrix.
    float s[16], factor, pivotModel[3], pivot[3];
    ReadBone(BoneSlot(buffer, first), s, &factor);
    const float pivotBind[3] = {0.0f, g_boneCentroid[first][1], g_boneCentroid[first][2]};
    PointTimes(pivotBind, s, pivotModel);
    PointTimes(pivotModel, w, pivot);
    float rot[9], t[16], wt4[16], tm[16];
    AxisRotation(up, angle, rot);
    PivotMove(rot, pivot, pivot, t);
    Mul4(w, t, wt4);
    Mul4(wt4, winv, tm);
    for (int b = first; b <= kMarineUpperLast; ++b)
        if (!IsMarineHandBone(b))
            MoveBone(buffer, b, tm);
    return true;
}

// Bone hide: the bones' rotation parts zeroed, so their vertices collapse onto the bone's translation.
bool HideBones(float* buffer)
{
    const int first = settings::HideBodyBonesFirst(), last = settings::HideBodyBonesLast();
    if (first <= 0 || last < first)
        return false;
    for (int b = first - 1; b < last && b < kMaxBones; ++b)
    {
        float* bone = buffer + kBonesOffset / 4 + b * 12; // 3 rows of float4
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k)
                bone[r * 4 + k] = 0.0f;
    }
    return true;
}

void Upload(ID3D11DeviceContext* c, const float* data)
{
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_realMap(c, g_boneBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) && m.pData)
    {
        std::memcpy(m.pData, data, kBoneBufferBytes);
        g_realUnmap(c, g_boneBuffer, 0);
    }
}

// A newly uploaded model's first draw: classify it, rewrite its bones / placement when asked.
void StartModel(ID3D11DeviceContext* c, ID3D11Buffer* vb)
{
    if (g_modelVB && g_modelFirstPerson)
        NoteFrameModel(g_modelVB, g_modelDraws);
    g_modelVB = vb;
    g_modelDraws = 0;

    float camRot[9], camPos[3];
    stereo::GameCamera(camRot, camPos);
    const float dx = g_snap[12] - camPos[0], dy = g_snap[13] - camPos[1], dz = g_snap[14] - camPos[2];
    const bool firstPerson = dx * dx + dy * dy + dz * dz < kFirstPersonRadius * kFirstPersonRadius;
    g_modelFirstPerson = firstPerson;
    g_role = !firstPerson ? Role::Other : vb == g_bodyVB ? Role::Body : g_bodyVB ? Role::Weapon : Role::Other;

    if (g_role == Role::Other)
        return;
    alignas(16) static float copy[kBoneBufferBytes / 4];
    std::memcpy(copy, g_snap, kBoneBufferBytes);
    bool changed = false;
    if (g_role == Role::Body)
    {
        if (settings::ArmsFollowControllers() && Global<int>(addr::g_PlayerSpecies) == 2)
        {
            changed = OffsetBody(copy) || changed; // first: the arms' IK starts from the moved, turned shoulders
            changed = TurnTorso(copy) || changed;
            changed = ArmToController(copy, 0, kMarineArms[0]) || changed;
            changed = ArmToController(copy, 1, kMarineArms[1]) || changed;
        }
        changed = HideBones(copy) || changed;
    }
    else if (settings::GunOnRightController())
        changed = PlaceOnController(copy);
    if (changed)
        Upload(c, copy);
}

// ---- Bone map: which bones the body's parts use, and where (bind space) ----
bool ReadBuffer(ID3D11Device* dev, ID3D11DeviceContext* c, ID3D11Buffer* src, std::vector<uint8_t>& out)
{
    D3D11_BUFFER_DESC d{};
    src->GetDesc(&d);
    D3D11_BUFFER_DESC s{};
    s.ByteWidth = d.ByteWidth;
    s.Usage = D3D11_USAGE_STAGING;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* staging = nullptr;
    if (FAILED(dev->CreateBuffer(&s, nullptr, &staging)) || !staging)
        return false;
    c->CopyResource(staging, src);
    D3D11_MAPPED_SUBRESOURCE m{};
    bool ok = false;
    if (SUCCEEDED(g_realMap(c, staging, 0, D3D11_MAP_READ, 0, &m)) && m.pData)
    {
        out.assign(static_cast<const uint8_t*>(m.pData), static_cast<const uint8_t*>(m.pData) + d.ByteWidth);
        g_realUnmap(c, staging, 0);
        ok = true;
    }
    staging->Release();
    return ok;
}

void LogBoneMap(ID3D11DeviceContext* c)
{
    ID3D11Device* dev = nullptr;
    c->GetDevice(&dev);
    if (!dev)
        return;
    std::vector<uint8_t> vb, ib;
    const bool ok = ReadBuffer(dev, c, g_bodyVB, vb) && ReadBuffer(dev, c, g_bodyIB, ib);
    dev->Release();
    if (!ok || g_bodyStride < 32)
    {
        AVP_LOG(kLog, "bone map: buffer read-back failed");
        return;
    }
    const size_t stride = g_bodyStride, vertexCount = vb.size() / stride;
    const bool index32 = g_bodyIndexFormat == DXGI_FORMAT_R32_UINT;
    auto indexAt = [&](size_t i) -> size_t {
        if (index32)
            return i * 4 + 4 <= ib.size() ? *reinterpret_cast<const uint32_t*>(&ib[i * 4]) : 0;
        return i * 2 + 2 <= ib.size() ? *reinterpret_cast<const uint16_t*>(&ib[i * 2]) : 0;
    };
    // The body's vertices (all parts), each once.
    std::vector<uint8_t> used(vertexCount, 0);
    std::vector<std::vector<uint8_t>> partOf(vertexCount); // part numbers (1-based) per vertex
    for (size_t p = 0; p < g_bodyParts.size(); ++p)
    {
        const Part& part = g_bodyParts[p];
        for (unsigned i = 0; i < part.count; ++i)
        {
            const size_t v = indexAt(part.start + i) + part.base;
            if (v >= vertexCount)
                continue;
            used[v] = 1;
            auto& list = partOf[v];
            if (list.empty() || list.back() != p + 1)
                list.push_back(static_cast<uint8_t>(p + 1));
        }
    }
    // Field detection. Weights: 4 values summing to 1 on (nearly) every used vertex, as bytes (UBYTE4N, sum 255),
    // 16-bit (SHORT4N 32767 / USHORT4N 65535 -- the Marine body: 8 bytes at +20) or floats. Indices: 4 values all < 128
    // on every used vertex, as 16-bit (tried first: read as bytes, 16-bit indices pass too, their high bytes being 0;
    // real byte indices fail as 16-bit, a second index lands in the high byte) or bytes.
    enum Fmt { kNone, kByte, kShort, kFloat };
    int weightOff = -1, indexOff = -1, weightFmt = kNone, indexFmt = kNone;
    double weightScale = 255.0;
    const unsigned maxBones = kMaxBones;
    auto u16 = [](const uint8_t* q, int k) { return static_cast<unsigned>(q[k * 2] | (q[k * 2 + 1] << 8)); };
    auto f32 = [](const uint8_t* q, int k) {
        float f;
        std::memcpy(&f, q + k * 4, 4);
        return f;
    };
    for (size_t off = 12; off + 4 <= stride; off += 4)
    {
        const bool has8 = off + 8 <= stride, has16 = off + 16 <= stride;
        size_t n = 0, sum255 = 0, sum32767 = 0, sum65535 = 0, sum1 = 0, bytesBelow = 0, shortsBelow = 0;
        bool nonzero = false;
        for (size_t v = 0; v < vertexCount; ++v)
        {
            if (!used[v])
                continue;
            const uint8_t* q = &vb[v * stride + off];
            ++n;
            const int sum = q[0] + q[1] + q[2] + q[3];
            if (sum >= 250 && sum <= 260)
                ++sum255;
            if (q[0] < kMaxBones && q[1] < kMaxBones && q[2] < kMaxBones && q[3] < kMaxBones)
                ++bytesBelow;
            nonzero = nonzero || q[0] || q[1] || q[2] || q[3];
            if (has8)
            {
                const unsigned s = u16(q, 0) + u16(q, 1) + u16(q, 2) + u16(q, 3);
                if (s >= 32760 && s <= 32775)
                    ++sum32767;
                if (s >= 65525 && s <= 65545)
                    ++sum65535;
                if (u16(q, 0) < maxBones && u16(q, 1) < maxBones && u16(q, 2) < maxBones && u16(q, 3) < maxBones)
                    ++shortsBelow;
            }
            if (has16)
            {
                const float a = f32(q, 0), b = f32(q, 1), cc = f32(q, 2), d = f32(q, 3);
                const bool inRange = a >= -0.001f && a <= 1.001f && b >= -0.001f && b <= 1.001f && cc >= -0.001f &&
                                     cc <= 1.001f && d >= -0.001f && d <= 1.001f;
                if (inRange && std::fabs(a + b + cc + d - 1.0f) < 0.01f)
                    ++sum1;
            }
        }
        if (!n)
            break;
        if (weightOff < 0 && (sum255 * 100 >= n * 98 || sum32767 * 100 >= n * 98 || sum65535 * 100 >= n * 98 ||
                              sum1 * 100 >= n * 98))
        {
            weightOff = static_cast<int>(off);
            if (sum255 * 100 >= n * 98)
                weightFmt = kByte, weightScale = 255.0;
            else if (sum32767 * 100 >= n * 98)
                weightFmt = kShort, weightScale = 32767.0;
            else if (sum65535 * 100 >= n * 98)
                weightFmt = kShort, weightScale = 65535.0;
            else
                weightFmt = kFloat, weightScale = 1.0;
        }
        else if (indexOff < 0 && nonzero && (shortsBelow == n || bytesBelow == n))
        {
            indexOff = static_cast<int>(off);
            indexFmt = shortsBelow == n ? kShort : kByte;
        }
    }
    auto weightAt = [&](const uint8_t* q, int k) -> double {
        const uint8_t* f = q + weightOff;
        return weightFmt == kByte ? f[k] / weightScale : weightFmt == kShort ? u16(f, k) / weightScale : f32(f, k);
    };
    auto boneAt = [&](const uint8_t* q, int k) -> unsigned {
        const uint8_t* f = q + indexOff;
        return indexFmt == kShort ? u16(f, k) : f[k];
    };
    static const char* const kFmtName[] = {"?", "bytes", "16-bit", "floats"};
    {
        const uint8_t* q = vb.data();
        char hex[3 * 64 + 1] = {};
        for (size_t i = 0; i < stride && i < 64; ++i)
            _snprintf_s(hex + i * 3, 4, _TRUNCATE, "%02X ", q[i]);
        AVP_LOG(kLog, "bone map: body VB %p (%zu vertices, stride %zu), %zu parts; first vertex: %s", g_bodyVB,
                vertexCount, stride, g_bodyParts.size(), hex);
    }
    AVP_LOG(kLog, "bone map: blend weights at +%d (%s, 1 = %.0f), blend indices at +%d (%s) (positions assumed float3 at "
            "+0)", weightOff, kFmtName[weightFmt], weightScale, indexOff, kFmtName[indexFmt]);
    if (weightOff < 0 || indexOff < 0)
        return;
    {
        // A few vertices with two or more bones, decoded, to confirm the formats.
        int shown = 0;
        for (size_t v = 0; v < vertexCount && shown < 4; ++v)
        {
            const uint8_t* q = &vb[v * stride];
            if (!used[v] || weightAt(q, 1) <= 0.0)
                continue;
            ++shown;
            AVP_LOG(kLog, "  vertex %zu: bones %u %u %u %u, weights %.3f %.3f %.3f %.3f", v, boneAt(q, 0), boneAt(q, 1),
                    boneAt(q, 2), boneAt(q, 3), weightAt(q, 0), weightAt(q, 1), weightAt(q, 2), weightAt(q, 3));
        }
    }
    struct Stat
    {
        double w = 0, x = 0, y = 0, z = 0;
        uint32_t parts = 0; // bit p-1 = part p
    };
    Stat stats[kMaxBones];
    for (size_t v = 0; v < vertexCount; ++v)
    {
        if (!used[v])
            continue;
        const uint8_t* q = &vb[v * stride];
        const float* p = reinterpret_cast<const float*>(q);
        for (int k = 0; k < 4; ++k)
        {
            const unsigned bone = boneAt(q, k);
            const double w = weightAt(q, k);
            if (w <= 0.0 || bone >= static_cast<unsigned>(kMaxBones))
                continue;
            Stat& s = stats[bone];
            s.w += w;
            s.x += w * p[0];
            s.y += w * p[1];
            s.z += w * p[2];
            for (uint8_t part : partOf[v])
                if (part <= 32)
                    s.parts |= 1u << (part - 1);
        }
    }
    for (int b = 0; b < kMaxBones; ++b)
    {
        const Stat& s = stats[b];
        if (s.w <= 0.0)
            continue;
        char parts[128] = {};
        int used2 = 0;
        for (int p = 0; p < 32; ++p)
            if (s.parts & (1u << p))
                used2 += _snprintf_s(parts + used2, sizeof(parts) - used2, _TRUNCATE, "%d ", p + 1);
        const double cx = s.x / s.w, cy = s.y / s.w, cz = s.z / s.w;
        AVP_LOG(kLog, "  bone %3d (hide # %3d): weight %7.1f, centroid (%7.3f %7.3f %7.3f) %s, parts %s", b, b + 1, s.w, cx,
                cy, cz, cx < -0.02 ? "x-" : cx > 0.02 ? "x+" : "x0", parts);
    }
    for (int b = 0; b < kMaxBones; ++b)
    {
        g_boneWeight[b] = static_cast<float>(stats[b].w);
        if (stats[b].w > 0.0)
        {
            g_boneCentroid[b][0] = static_cast<float>(stats[b].x / stats[b].w);
            g_boneCentroid[b][1] = static_cast<float>(stats[b].y / stats[b].w);
            g_boneCentroid[b][2] = static_cast<float>(stats[b].z / stats[b].w);
        }
    }
    g_haveCentroids = true;
    g_armLogged[0] = g_armLogged[1] = false;

    // Each arm's joints: the centre of the vertices skinned across a joint's two segments, each vertex weighted by the
    // smaller of its two shares. Joint j joins segments j and j + 1: shoulder (other / upper), elbow (upper / fore),
    // wrist (fore / hand).
    for (int arm = 0; arm < 2; ++arm)
    {
        const ArmRig& r = kMarineArms[arm];
        double joint[3][4] = {}, seg[4][4] = {}; // x y z, weight
        for (size_t v = 0; v < vertexCount; ++v)
        {
            if (!used[v])
                continue;
            const uint8_t* q = &vb[v * stride];
            const float* p = reinterpret_cast<const float*>(q);
            double share[4] = {};
            for (int k = 0; k < 4; ++k)
            {
                const unsigned bone = boneAt(q, k);
                const double w = weightAt(q, k);
                if (w > 0.0 && bone < static_cast<unsigned>(kMaxBones))
                    share[SegmentOf(r, static_cast<int>(bone))] += w;
            }
            for (int s = 0; s < 4; ++s)
            {
                for (int k = 0; k < 3; ++k)
                    seg[s][k] += share[s] * p[k];
                seg[s][3] += share[s];
            }
            for (int j = 0; j < 3; ++j)
            {
                const double w = share[j] < share[j + 1] ? share[j] : share[j + 1];
                for (int k = 0; k < 3; ++k)
                    joint[j][k] += w * p[k];
                joint[j][3] += w;
            }
        }
        bool ok = seg[kSegUpper][3] > 0.0 && seg[kSegFore][3] > 0.0 && seg[kSegHand][3] > 0.0;
        for (int j = 1; j < 3 && ok; ++j)
            ok = joint[j][3] >= 1.0;
        if (ok)
        {
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    g_armJoint[arm][j][k] = static_cast<float>(joint[j][k] / joint[j][3]);
            if (joint[0][3] < 1.0) // no blend at the shoulder: the elbow mirrored through the upper arm's centre
                for (int k = 0; k < 3; ++k)
                    g_armJoint[arm][0][k] =
                        static_cast<float>(2.0 * seg[kSegUpper][k] / seg[kSegUpper][3]) - g_armJoint[arm][1][k];
        }
        g_haveJoints[arm] = ok;
        const float(*jt)[3] = g_armJoint[arm];
        auto dist = [](const float* a, const float* b) {
            return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
        };
        AVP_LOG(kLog, "  %s arm joints%s: shoulder (%.3f %.3f %.3f)%s, elbow (%.3f %.3f %.3f), wrist (%.3f %.3f %.3f); "
                "upper arm %.3f, forearm %.3f (blend weights %.1f / %.1f / %.1f)", arm ? "right" : "left",
                ok ? "" : " NOT FOUND (rigid arm)", jt[0][0], jt[0][1], jt[0][2], joint[0][3] < 1.0 ? " (estimated)" : "",
                jt[1][0], jt[1][1], jt[1][2], jt[2][0], jt[2][1], jt[2][2], dist(jt[0], jt[1]), dist(jt[1], jt[2]),
                joint[0][3], joint[1][3], joint[2][3]);
    }
}

// ---- Draw signatures (debug MeshProbe) ----
void ProbeDraw(ID3D11DeviceContext* c, unsigned drawNo, unsigned indexCount, unsigned startIndex, int baseVertex)
{
    static DWORD lastLog[kMaxProbeDraw + 1] = {};
    const DWORD now = GetTickCount();
    if (drawNo > kMaxProbeDraw || (lastLog[drawNo] && now - lastLog[drawNo] < 5000))
        return;
    lastLog[drawNo] = now;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT ibOff = 0;
    c->IAGetIndexBuffer(&ib, &fmt, &ibOff);
    AVP_LOG(kLog, "draw #%u: %s, model part %u, %u indices from %u (base %d), IB %p, VB %p, world (%.2f %.2f %.2f)", drawNo,
            RoleName(g_role), g_modelDraws, indexCount, startIndex, baseVertex, ib, g_modelVB, g_snap[12], g_snap[13],
            g_snap[14]);
    if (ib)
        ib->Release();
}

} // namespace

void Install(ID3D11DeviceContext* ctx)
{
    g_ctx = ctx;
    for (int j = 0; j < kHudPassCount; ++j)
        if (kHudPasses[j].returnAddress == kBodyPassReturn)
            g_bodyPass = j;
    HookEntry(ctx, kCtxMap, reinterpret_cast<void*>(&Hook_Map), reinterpret_cast<void**>(&g_realMap), "Map");
    HookEntry(ctx, kCtxUnmap, reinterpret_cast<void*>(&Hook_Unmap), reinterpret_cast<void**>(&g_realUnmap), "Unmap");
    HookEntry(ctx, kCtxUpdateSubresource, reinterpret_cast<void*>(&Hook_UpdateSubresource),
              reinterpret_cast<void**>(&g_realUpdateSubresource), "UpdateSubresource");
    AVP_LOG(kLog, "first-person module ready: body pass index %d", g_bodyPass);
}

void OnDraw(ID3D11DeviceContext* c, int pass, unsigned drawNo, unsigned indexCount, unsigned startIndex,
            int baseVertex, bool secondRender)
{
    if (pass != g_bodyPass || pass < 0)
        return;
    if (!g_boneBuffer)
    {
        // The bone buffer: VS constant buffer 5 of this pass's skinned draws.
        ID3D11Buffer* cb = nullptr;
        c->VSGetConstantBuffers(5, 1, &cb);
        if (cb)
        {
            D3D11_BUFFER_DESC d{};
            cb->GetDesc(&d);
            if (d.ByteWidth == kBoneBufferBytes)
            {
                g_boneBuffer = cb;
                AVP_LOG(kLog, "bone buffer %p (%u bytes); the next upload is the first one seen", cb, d.ByteWidth);
            }
            cb->Release();
        }
        return; // no snapshot yet for this model
    }
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, vbOffset = 0;
    c->IAGetVertexBuffers(0, 1, &vb, &stride, &vbOffset);
    if (vb)
        vb->Release(); // compared only
    if (g_fillSeq != g_handledFill)
    {
        g_handledFill = g_fillSeq;
        StartModel(c, vb);
    }
    ++g_modelDraws;
    if (g_role == Role::Body && !secondRender && g_bodyVB != g_mappedBodyVB)
    {
        // Collect the body's parts this frame for the bone map (done at Present).
        if (g_modelDraws == 1)
            g_bodyParts.clear();
        g_bodyParts.push_back({startIndex, indexCount, baseVertex});
        ID3D11Buffer* ib = nullptr;
        UINT ibOff = 0;
        c->IAGetIndexBuffer(&ib, &g_bodyIndexFormat, &ibOff);
        if (ib)
            ib->Release();
        g_bodyIB = ib;
        g_bodyStride = stride;
    }
    if (settings::DebugMeshProbe() && !secondRender)
        ProbeDraw(c, drawNo, indexCount, startIndex, baseVertex);
}

void OnPresent(ID3D11DeviceContext* ctx)
{
    if (g_modelVB && g_modelFirstPerson)
        NoteFrameModel(g_modelVB, g_modelDraws);
    // The body = the first-person model with the most parts this frame.
    ID3D11Buffer* body = nullptr;
    unsigned most = 0;
    for (int i = 0; i < g_frameModelCount; ++i)
        if (g_frameModels[i].draws > most)
        {
            most = g_frameModels[i].draws;
            body = g_frameModels[i].vb;
        }
    if (body && body != g_bodyVB && most >= 4)
    {
        AVP_LOG(kLog, "body model: VB %p (%u parts); %d first-person model(s) this frame", body, most, g_frameModelCount);
        g_bodyVB = body;
        g_bodyParts.clear();
        g_haveCentroids = false; // the bone map's centroids and joints are the previous body's
        g_haveJoints[0] = g_haveJoints[1] = false;
        g_bodyForwardSign = 0.0f;
    }
    g_frameModelCount = 0;
    g_modelVB = nullptr;
    g_modelFirstPerson = false;
    g_role = Role::Other;
    // The bone map, once per body, after a frame that drew all its parts.
    if (ctx && g_bodyVB && g_bodyVB != g_mappedBodyVB && g_bodyIB && g_bodyParts.size() >= 4)
    {
        g_mappedBodyVB = g_bodyVB;
        LogBoneMap(ctx);
    }
}

} // namespace avp::fp
