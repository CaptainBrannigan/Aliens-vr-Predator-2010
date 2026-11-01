#include "holsters.h"

#include "grab_view.h"
#include "stereo.h"
#include "vr_input.h"
#include "../core/addresses.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../hooks/first_person.h"

#include <windows.h>

#include <atomic>
#include <cmath>

namespace avp::holsters {
namespace {

constexpr const char* kLog = "avpvr_holsters.log";
constexpr unsigned kLeftBumper = 0x0100, kRightBumper = 0x0200;
constexpr unsigned kDpadMask = 0x000F;     // the Marine's weapon selection
constexpr ULONGLONG kPressMs = 150;        // the game samples the pad at its tick rate
constexpr float kBodyYawSlackDeg = 40.0f;  // a glance to the side doesn't swing the holsters

struct Zone
{
    const char* name;
    int zone;     // settings::kHolster*
    float mirror; // +1 / -1 on the setting's right for the mirrored pairs, 0 = as set
};
constexpr Zone kZones[] = {
    {"stomach left", settings::kHolsterStomach, -1.0f},
    {"stomach right", settings::kHolsterStomach, 1.0f},
    {"hip left", settings::kHolsterHip, -1.0f},
    {"hip right", settings::kHolsterHip, 1.0f},
    {"chest", settings::kHolsterChest, 0.0f},
    {"left shoulder", settings::kHolsterLeftShoulder, 0.0f},
    {"right shoulder", settings::kHolsterRightShoulder, 0.0f},
};

// Main thread.
bool g_haveBody = false;
float g_bodyYaw = 0.0f, g_bodyForward[2] = {}; // atan2(x, z); x, z of the horizontal body forward, tracking space
bool g_prevGrip[2] = {};
bool g_gripFromZone[2] = {}; // this grip press took a spot: its bumper stays silent until it's let go
int g_grabbedBy = -1;
bool g_lastSetting = false;

std::atomic<int> g_handOverride{-1}; // 0 left, 1 right, -1 = the setting
std::atomic<unsigned> g_suppress{0}, g_pressMask{0};
std::atomic<ULONGLONG> g_pressUntil{0};

float WrapPi(float a)
{
    constexpr float kPi = 3.14159265f;
    while (a > kPi)
        a -= 2.0f * kPi;
    while (a <= -kPi)
        a += 2.0f * kPi;
    return a;
}

void UpdateBody(const float forward[3])
{
    const float len = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (len < 1e-3f)
        return;
    const float headYaw = std::atan2(forward[0] / len, forward[2] / len);
    if (!g_haveBody)
    {
        g_bodyYaw = headYaw;
        g_haveBody = true;
    }
    const float diff = WrapPi(headYaw - g_bodyYaw), slack = kBodyYawSlackDeg * 0.01745329252f;
    if (diff > slack)
        g_bodyYaw = WrapPi(g_bodyYaw + diff - slack);
    else if (diff < -slack)
        g_bodyYaw = WrapPi(g_bodyYaw + diff + slack);
    g_bodyForward[0] = std::sin(g_bodyYaw);
    g_bodyForward[1] = std::cos(g_bodyYaw);
}

// A spot's centre, tracking space.
void ZoneCentre(const Zone& z, const float head[3], float c[3])
{
    // forward x up = right for a right-handed, Y-up frame.
    const float rightX = -g_bodyForward[1], rightZ = g_bodyForward[0];
    float off[3];
    settings::HolsterOffset(z.zone, off);
    const float r = z.mirror != 0.0f ? z.mirror * std::fabs(off[0]) : off[0];
    c[0] = head[0] + rightX * r + g_bodyForward[0] * off[2];
    c[1] = head[1] + off[1] - settings::HolsterDrop();
    c[2] = head[2] + rightZ * r + g_bodyForward[1] * off[2];
}

// The nearest spot within the holster size that has an action, or nullptr; *nearest = the nearest spot of any size.
const Zone* FindZone(const float hand[3], const float head[3], float* nearest)
{
    const float radius = settings::HolsterRadius();
    const Zone* best = nullptr;
    float bestSq = radius * radius, nearestSq = 1e9f;
    for (const Zone& z : kZones)
    {
        float c[3];
        ZoneCentre(z, head, c);
        const float dx = hand[0] - c[0], dy = hand[1] - c[1], dz = hand[2] - c[2];
        const float sq = dx * dx + dy * dy + dz * dz;
        if (sq < nearestSq)
            nearestSq = sq;
        if (sq <= bestSq && settings::HolsterActionMask(z.zone) != 0)
        {
            bestSq = sq;
            best = &z;
        }
    }
    *nearest = std::sqrt(nearestSq);
    return best;
}

const char* HandName(int hand) { return hand ? "right" : "left"; }

// The grab zone view (vr/grab_view.h).
void PublishView(bool zones, const float head[3])
{
    grabview::Shape shapes[grabview::kSlotCount];
    float hands[2][3];
    bool have[2];
    for (int h = 0; h < 2; ++h)
    {
        float pose[3][4];
        have[h] = input::GetHandPose(h, true, pose);
        for (int k = 0; k < 3 && have[h]; ++k)
            hands[h][k] = pose[k][3];
    }
    const float radius = settings::HolsterRadius();
    for (int i = 0; i < grabview::kZoneSlots && zones; ++i)
    {
        grabview::Shape& s = shapes[i];
        ZoneCentre(kZones[i], head, s.pos);
        s.visible = s.billboard = true;
        s.width = s.height = radius * 2.0f;
        s.assigned = settings::HolsterActionMask(kZones[i].zone) != 0;
        for (int h = 0; h < 2; ++h)
            if (have[h])
            {
                const float dx = hands[h][0] - s.pos[0], dy = hands[h][1] - s.pos[1], dz = hands[h][2] - s.pos[2];
                s.active = s.active || dx * dx + dy * dy + dz * dz <= radius * radius;
            }
    }
    // The two-handed grab: a band along the barrel, its flat side to the head.
    float start[3], end[3], world[2][3], grab;
    if (fp::TwoHandBand(world[0], world[1], &grab) && stereo::WorldToTracking(world[0], start) &&
        stereo::WorldToTracking(world[1], end))
    {
        float x[3], z[3];
        grabview::Shape& s = shapes[grabview::kTwoHandSlot];
        for (int k = 0; k < 3; ++k)
            x[k] = end[k] - start[k];
        const float length = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        if (length > 1e-3f)
        {
            for (float& v : x)
                v /= length;
            float middle[3], along = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                middle[k] = 0.5f * (start[k] + end[k]);
                z[k] = head[k] - middle[k];
                along += z[k] * x[k];
            }
            for (int k = 0; k < 3; ++k)
                z[k] -= along * x[k];
            const float zl = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
            if (zl > 1e-3f)
            {
                for (float& v : z)
                    v /= zl;
                const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
                const float* axes[3] = {x, y, z};
                for (int col = 0; col < 3; ++col)
                    for (int row = 0; row < 3; ++row)
                        s.m[row * 4 + col] = axes[col][row];
                for (int row = 0; row < 3; ++row)
                    s.m[row * 4 + 3] = middle[row];
                s.visible = true;
                s.active = fp::LeftGripOnGun();
                s.width = length;
                s.height = grab * 2.0f;
            }
        }
    }
    grabview::Update(shapes, head);
}

} // namespace

void Update()
{
    const bool marine = Global<int>(addr::g_PlayerSpecies) == 2;
    const bool setting = settings::LeftHanded();
    if (setting != g_lastSetting || !marine || !settings::HolsterGrabHand())
    {
        g_lastSetting = setting;
        g_handOverride.store(-1, std::memory_order_relaxed);
        g_grabbedBy = -1;
    }
    float headPos[3], headForward[3];
    const bool haveHead = stereo::StereoActive() && stereo::HeadPose(headPos, headForward);
    const bool active = settings::Holsters() && haveHead;
    if (haveHead)
        UpdateBody(headForward); // holsters off too: the flashlight's chest mount
    const bool holdToKeep = settings::HolsterHoldToKeep();
    static int missLogs = 0;
    for (int h = 0; h < 2; ++h)
    {
        const bool grip = input::GripHeld(h);
        const bool pressed = grip && !g_prevGrip[h], released = !grip && g_prevGrip[h];
        g_prevGrip[h] = grip;
        if (released)
        {
            g_gripFromZone[h] = false;
            if (holdToKeep && g_grabbedBy == h)
            {
                g_handOverride.store(-1, std::memory_order_relaxed);
                AVP_LOG(kLog, "weapon back in the default hand (grip released, hold to keep)");
            }
            if (g_grabbedBy == h)
                g_grabbedBy = -1;
        }
        float pose[3][4];
        const bool gunHand = h == (LeftHandedNow() ? 0 : 1);
        const Zone* zone = nullptr;
        float nearest = 0.0f;
        if (active && g_haveBody && input::GetHandPose(h, true, pose))
        {
            const float hand[3] = {pose[0][3], pose[1][3], pose[2][3]};
            // The off hand on the gun is the two-handed grab.
            if (gunHand || !fp::LeftGripOnGun())
                zone = FindZone(hand, headPos, &nearest);
        }
        if (!pressed)
            continue;
        if (!zone)
        {
            if (active && missLogs < 60)
            {
                ++missLogs;
                AVP_LOG(kLog, "%s grip in no holster: nearest spot %.2f m (size %.2f m)", HandName(h), nearest,
                        settings::HolsterRadius());
            }
            continue;
        }
        const unsigned mask = settings::HolsterActionMask(zone->zone);
        g_gripFromZone[h] = true;
        g_pressMask.store(mask, std::memory_order_relaxed);
        g_pressUntil.store(GetTickCount64() + kPressMs, std::memory_order_relaxed);
        const bool weapon = marine && (mask & kDpadMask) != 0 && settings::HolsterGrabHand();
        AVP_LOG(kLog, "%s hand grip in %s: pad 0x%04X%s", HandName(h), zone->name, mask,
                weapon ? ", weapon now in this hand" : "");
        if (weapon)
        {
            g_handOverride.store(h, std::memory_order_relaxed);
            g_grabbedBy = h;
        }
    }
    // Every frame: a hand in a spot (its grip is about to be the grab) or holding one keeps its bumper silent.
    unsigned suppress = 0;
    const bool left = LeftHandedNow();
    for (int h = 0; h < 2; ++h)
    {
        float pose[3][4], nearest;
        bool hold = g_gripFromZone[h];
        if (!hold && active && g_haveBody && input::GetHandPose(h, true, pose))
        {
            const float hand[3] = {pose[0][3], pose[1][3], pose[2][3]};
            hold = (h == (left ? 0 : 1) || !fp::LeftGripOnGun()) && FindZone(hand, headPos, &nearest) != nullptr;
        }
        if (hold)
            suppress |= (h == 0) != left ? kLeftBumper : kRightBumper;
    }
    g_suppress.store(suppress, std::memory_order_relaxed);
    if (settings::ShowGrabZones() && stereo::StereoActive() && stereo::HeadPose(headPos, headForward))
        PublishView(active && g_haveBody, headPos);
}

bool ChestPose(float m[3][4])
{
    float head[3], forward[3], c[3];
    if (!g_haveBody || !stereo::HeadPose(head, forward))
        return false;
    for (const Zone& z : kZones)
        if (z.zone == settings::kHolsterChest)
            ZoneCentre(z, head, c);
    const float f[3] = {g_bodyForward[0], 0.0f, g_bodyForward[1]};
    const float right[3] = {-f[2], 0.0f, f[0]}; // forward x up
    for (int i = 0; i < 3; ++i)
    {
        m[i][0] = right[i];
        m[i][1] = i == 1 ? 1.0f : 0.0f;
        m[i][2] = -f[i]; // a pose looks along its -z
        m[i][3] = c[i];
    }
    return true;
}

bool LeftHandedNow()
{
    const int hand = g_handOverride.load(std::memory_order_relaxed);
    return hand >= 0 ? hand == 0 : settings::LeftHanded();
}

unsigned short Apply(unsigned short buttons)
{
    unsigned b = buttons & ~g_suppress.load(std::memory_order_relaxed);
    if (GetTickCount64() < g_pressUntil.load(std::memory_order_relaxed))
        b |= g_pressMask.load(std::memory_order_relaxed);
    return static_cast<unsigned short>(b);
}

} // namespace avp::holsters
