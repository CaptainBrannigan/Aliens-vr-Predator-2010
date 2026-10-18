#include "settings.h"

#include "log.h"
#include "../hooks/hud_passes.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace avp::settings {
namespace {

constexpr const char* kLog = "avpvr_settings.log";
constexpr const char* kFileName = "avpvr_settings.ini";

enum class Kind
{
    Toggle,
    Number,
    Choice,
};

struct Entry
{
    const char* key; // ini name
    const char* group;
    const char* label;
    Kind kind;
    float defaultValue;
    float minValue, maxValue, step; // Number; Choice: 0 .. count - 1
    int decimals;
    const char* unit;
    const char* (*choiceLabel)(int); // Choice only
};

// "Debug: hide pass" choices: 0 = off, i = kHudPasses[i - 1].
// "Debug: hide pass" choices: 0 = off, i = kHudPasses[i - 1], kHudPassCount + 1 = unlisted draws.
const char* HidePassLabel(int i) { return i <= 0 ? "off" : HudPassLabel(i - 1); }

// Order = display order; entries of a group must be contiguous.
const Entry kEntries[] = {
    {"IpdScale", "View", "Eye distance (IPD) scale", Kind::Number, 1.0f, 0.5f, 2.0f, 0.02f, 2, "x", nullptr},
    {"GameCameraBase", "View", "Keep the game camera's motion (shake, recoil)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "",
     nullptr},
    {"RollFromHeadOnly", "View", "Camera roll from your head only", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"EyeFovBetweenFrames", "View", "Wide culling between frames (less pop-in)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "",
     nullptr},
    {"HudPanel", "HUD", "HUD in a panel (off = full view)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"HudWidthDeg", "HUD", "HUD size (width)", Kind::Number, 60.0f, 20.0f, 110.0f, 1.0f, 0, "deg", nullptr},
    {"HudDistance", "HUD", "HUD distance", Kind::Number, 2.5f, 0.5f, 20.0f, 0.1f, 1, "m", nullptr},
    {"HudHeightDeg", "HUD", "HUD height (+ up)", Kind::Number, 0.0f, -30.0f, 30.0f, 0.5f, 1, "deg", nullptr},
    {"HideDuplicateTracker", "HUD", "Hide the duplicate motion tracker", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"HideMarineReticle", "HUD", "Hide the reticle (Marine)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"HeadAim", "Controls", "Head aim (your head aims)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"TurnSpeedDeg", "Controls", "Turn speed (right stick)", Kind::Number, 150.0f, 30.0f, 360.0f, 5.0f, 0, "deg/s",
     nullptr},
    {"StickFullDeflection", "Controls", "Stick counts as full at", Kind::Number, 0.95f, 0.5f, 1.0f, 0.01f, 2, "",
     nullptr},
    {"TurnSpeed", "Controls", "Game look speed (head aim off)", Kind::Number, 1.0f, 0.2f, 3.0f, 0.05f, 2, "x",
     nullptr},
    {"MarineAimUp", "Aim offsets", "Marine aim up", Kind::Number, 0.0f, -15.0f, 15.0f, 0.25f, 2, "deg", nullptr},
    {"MarineAimRight", "Aim offsets", "Marine aim right", Kind::Number, 0.0f, -15.0f, 15.0f, 0.25f, 2, "deg", nullptr},
    {"AlienAimUp", "Aim offsets", "Alien aim up", Kind::Number, 0.0f, -15.0f, 15.0f, 0.25f, 2, "deg", nullptr},
    {"AlienAimRight", "Aim offsets", "Alien aim right", Kind::Number, 0.0f, -15.0f, 15.0f, 0.25f, 2, "deg", nullptr},
    {"PredatorAimUp", "Aim offsets", "Predator aim up", Kind::Number, 0.0f, -15.0f, 15.0f, 0.25f, 2, "deg", nullptr},
    {"PredatorAimRight", "Aim offsets", "Predator aim right", Kind::Number, 0.0f, -15.0f, 15.0f, 0.25f, 2, "deg",
     nullptr},
    {"ArmsFollowControllers", "Hands", "Arms follow the controllers (Marine)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "",
     nullptr},
    {"BodyForward", "Hands", "Body (shoulders) forward", Kind::Number, 0.05f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"BodyUp", "Hands", "Body (shoulders) up", Kind::Number, -0.06f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"StomachTurn", "Hands", "Torso turn from the stomach (+ right)", Kind::Number, -10.0f, -90.0f, 90.0f, 1.0f, 0,
     "deg", nullptr},
    {"ElbowSwivel", "Hands", "Elbow swivel (+ lowers the elbows)", Kind::Number, 30.0f, -90.0f, 90.0f, 1.0f, 0, "deg",
     nullptr},
    {"GunOnRightController", "Weapon", "Gun on the right controller (Marine)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "",
     nullptr},
    {"GunOffsetRight", "Weapon", "Gun offset right", Kind::Number, 0.0f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"GunOffsetUp", "Weapon", "Gun offset up", Kind::Number, 0.0f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"GunOffsetForward", "Weapon", "Gun offset forward", Kind::Number, 0.0f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"GunPitch", "Weapon", "Gun pitch (+ up)", Kind::Number, -60.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"GunYaw", "Weapon", "Gun yaw (+ right)", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"GunRoll", "Weapon", "Gun roll", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"ShotsFromGun", "Aiming", "Shots from the gun (Marine)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"LaserSight", "Aiming", "Laser along the shots", Kind::Toggle, 0.0f, 0, 0, 0, 0, "", nullptr},
    {"ShotOriginRight", "Aiming", "Shot origin right", Kind::Number, -0.7f, -30.0f, 30.0f, 0.1f, 1, "cm", nullptr},
    {"ShotOriginUp", "Aiming", "Shot origin up", Kind::Number, 11.0f, -30.0f, 30.0f, 0.1f, 1, "cm", nullptr},
    {"ShotOriginForward", "Aiming", "Shot origin forward", Kind::Number, 12.0f, -30.0f, 60.0f, 0.1f, 1, "cm", nullptr},
    {"ShotPitch", "Aiming", "Shot pitch (+ up)", Kind::Number, 3.5f, -30.0f, 30.0f, 0.25f, 2, "deg", nullptr},
    {"ShotYaw", "Aiming", "Shot yaw (+ right)", Kind::Number, -1.75f, -30.0f, 30.0f, 0.25f, 2, "deg", nullptr},
    {"TwoHandedAim", "Aiming", "Two-handed aim (left grip on the gun)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"TwoHandGrabRadius", "Aiming", "Two-handed: grab distance from the barrel", Kind::Number, 15.0f, 5.0f, 40.0f, 1.0f,
     0, "cm", nullptr},
    {"TwoHandReach", "Aiming", "Two-handed: reach along the barrel", Kind::Number, 80.0f, 20.0f, 120.0f, 1.0f, 0, "cm",
     nullptr},
    {"SecondaryGripRight", "Aiming", "Two-handed: off-hand grip right", Kind::Number, 0.0f, -20.0f, 20.0f, 0.5f, 1, "cm",
     nullptr},
    {"SecondaryGripUp", "Aiming", "Two-handed: off-hand grip up", Kind::Number, 0.0f, -20.0f, 20.0f, 0.5f, 1, "cm",
     nullptr},
    {"LeftHandRight", "Left hand", "Left hand offset right", Kind::Number, 0.0f, -50.0f, 50.0f, 0.1f, 1, "cm", nullptr},
    {"LeftHandUp", "Left hand", "Left hand offset up", Kind::Number, -1.0f, -50.0f, 50.0f, 0.1f, 1, "cm", nullptr},
    {"LeftHandForward", "Left hand", "Left hand offset forward", Kind::Number, -1.0f, -50.0f, 50.0f, 0.1f, 1, "cm",
     nullptr},
    {"LeftHandPitch", "Left hand", "Left hand pitch (+ up)", Kind::Number, -46.0f, -180.0f, 180.0f, 1.0f, 0, "deg",
     nullptr},
    {"LeftHandYaw", "Left hand", "Left hand yaw (+ right)", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg",
     nullptr},
    {"LeftHandRoll", "Left hand", "Left hand roll", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"RightHandRight", "Right hand", "Right hand offset right", Kind::Number, 0.0f, -50.0f, 50.0f, 0.1f, 1, "cm",
     nullptr},
    {"RightHandUp", "Right hand", "Right hand offset up", Kind::Number, -1.3f, -50.0f, 50.0f, 0.1f, 1, "cm", nullptr},
    {"RightHandForward", "Right hand", "Right hand offset forward", Kind::Number, -1.5f, -50.0f, 50.0f, 0.1f, 1, "cm",
     nullptr},
    {"RightHandPitch", "Right hand", "Right hand pitch (+ up)", Kind::Number, -58.0f, -180.0f, 180.0f, 1.0f, 0, "deg",
     nullptr},
    {"RightHandYaw", "Right hand", "Right hand yaw (+ right)", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg",
     nullptr},
    {"RightHandRoll", "Right hand", "Right hand roll", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"HideBodyBonesFirst", "Debug", "Hide body bones from #", Kind::Number, 0.0f, 0.0f, 128.0f, 1.0f, 0, "", nullptr},
    {"HideBodyBonesLast", "Debug", "Hide body bones to #", Kind::Number, 0.0f, 0.0f, 128.0f, 1.0f, 0, "", nullptr},
    {"DebugMeshProbe", "Debug", "Mesh probe (log the body pass's draws)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"DebugHidePass", "Debug", "Hide pass", Kind::Choice, 0.0f, 0.0f, static_cast<float>(kHudPassCount + 1), 1.0f, 0, "",
     &HidePassLabel},
    {"DebugHideDraw", "Debug", "Hide only draw # of it (0 = all)", Kind::Number, 0.0f, 0.0f, 999.0f, 1.0f, 0, "",
     nullptr},

};
constexpr int kCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));

enum Index
{
    kIpdScale,
    kGameCameraBase,
    kRollFromHeadOnly,
    kEyeFovBetweenFrames,
    kHudPanel,
    kHudWidthDeg,
    kHudDistance,
    kHudHeightDeg,
    kHideDuplicateTracker,
    kHideMarineReticle,
    kHeadAim,
    kTurnSpeedDeg,
    kStickFullDeflection,
    kTurnSpeed,
    kMarineAimUp,
    kMarineAimRight,
    kAlienAimUp,
    kAlienAimRight,
    kPredatorAimUp,
    kPredatorAimRight,
    kArmsFollowControllers,
    kBodyForward,
    kBodyUp,
    kStomachTurn,
    kElbowSwivel,
    kGunOnRightController,
    kGunOffsetRight,
    kGunOffsetUp,
    kGunOffsetForward,
    kGunPitch,
    kGunYaw,
    kGunRoll,
    kShotsFromGun,
    kLaserSight,
    kShotOriginRight,
    kShotOriginUp,
    kShotOriginForward,
    kShotPitch,
    kShotYaw,
    kTwoHandedAim,
    kTwoHandGrabRadius,
    kTwoHandReach,
    kSecondaryGripRight,
    kSecondaryGripUp,
    kLeftHandRight,
    kLeftHandUp,
    kLeftHandForward,
    kLeftHandPitch,
    kLeftHandYaw,
    kLeftHandRoll,
    kRightHandRight,
    kRightHandUp,
    kRightHandForward,
    kRightHandPitch,
    kRightHandYaw,
    kRightHandRoll,
    kHideBodyBonesFirst,
    kHideBodyBonesLast,
    kDebugMeshProbe,
    kDebugHidePass,
    kDebugHideDraw,
};

std::atomic<float> g_values[kCount];
std::mutex g_fileMutex;

std::string FilePath() { return ModuleDirectory() + kFileName; }

float Clamp(const Entry& e, float v)
{
    if (e.kind == Kind::Toggle)
        return v != 0.0f ? 1.0f : 0.0f;
    if (v < e.minValue)
        v = e.minValue;
    if (v > e.maxValue)
        v = e.maxValue;
    return e.kind == Kind::Choice ? std::floor(v + 0.5f) : v;
}

void WriteFileLocked()
{
    std::string text = "; AvP VR settings -- change them in the SteamVR dashboard (AvP VR tab); this file is rewritten on "
                       "every change.\r\n";
    char line[160];
    for (int i = 0; i < kCount; ++i)
    {
        const Entry& e = kEntries[i];
        const float v = g_values[i].load(std::memory_order_relaxed);
        if (e.kind == Kind::Toggle)
            snprintf(line, sizeof(line), "%s=%d\r\n", e.key, v != 0.0f ? 1 : 0);
        else if (e.kind == Kind::Choice)
            snprintf(line, sizeof(line), "%s=%d\r\n", e.key, static_cast<int>(v));
        else
            snprintf(line, sizeof(line), "%s=%.*f\r\n", e.key, e.decimals + 2, v);
        text += line;
    }
    // Raw Win32 like the logger (no CRT streams that could be mid-write at exit).
    HANDLE h = CreateFileA(FilePath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        AVP_LOG(kLog, "could not write %s (%lu)", FilePath().c_str(), GetLastError());
        return;
    }
    DWORD written = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
}

} // namespace

void Load()
{
    std::lock_guard<std::mutex> lock(g_fileMutex);
    for (int i = 0; i < kCount; ++i)
        g_values[i].store(kEntries[i].defaultValue, std::memory_order_relaxed);

    std::string content;
    HANDLE h = CreateFileA(FilePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
            content.append(buf, got);
        CloseHandle(h);
    }
    int loaded = 0;
    size_t pos = 0;
    while (pos < content.size())
    {
        size_t end = content.find('\n', pos);
        std::string line = content.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? content.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == ';' || eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        for (int i = 0; i < kCount; ++i)
            if (key == kEntries[i].key)
            {
                g_values[i].store(Clamp(kEntries[i], static_cast<float>(atof(line.c_str() + eq + 1))),
                                  std::memory_order_relaxed);
                ++loaded;
            }
    }
    AVP_LOG(kLog, "%s: %d of %d values read%s", FilePath().c_str(), loaded, kCount,
            content.empty() ? " (no file: defaults)" : "");
    for (int i = 0; i < kCount; ++i)
        AVP_LOG(kLog, "  %s = %.3f", kEntries[i].key, g_values[i].load(std::memory_order_relaxed));
    // Rewrite: adds new settings, drops obsolete ones, normalizes the format.
    WriteFileLocked();
}

float IpdScale() { return g_values[kIpdScale].load(std::memory_order_relaxed); }
bool HudPanel() { return g_values[kHudPanel].load(std::memory_order_relaxed) != 0.0f; }
float HudWidthDeg() { return g_values[kHudWidthDeg].load(std::memory_order_relaxed); }
float HudDistance() { return g_values[kHudDistance].load(std::memory_order_relaxed); }
float HudHeightDeg() { return g_values[kHudHeightDeg].load(std::memory_order_relaxed); }

bool GameCameraBase() { return g_values[kGameCameraBase].load(std::memory_order_relaxed) != 0.0f; }
bool EyeFovBetweenFrames() { return g_values[kEyeFovBetweenFrames].load(std::memory_order_relaxed) != 0.0f; }
bool RollFromHeadOnly() { return g_values[kRollFromHeadOnly].load(std::memory_order_relaxed) != 0.0f; }
bool HideMarineReticle() { return g_values[kHideMarineReticle].load(std::memory_order_relaxed) != 0.0f; }

void AimOffsetDeg(int species, float* up, float* right)
{
    // g_PlayerSpecies: 0 Predator, 1 Alien, 2 Marine.
    const int u = species == 0 ? kPredatorAimUp : species == 1 ? kAlienAimUp : species == 2 ? kMarineAimUp : -1;
    *up = u >= 0 ? g_values[u].load(std::memory_order_relaxed) : 0.0f;
    *right = u >= 0 ? g_values[u + 1].load(std::memory_order_relaxed) : 0.0f;
}

bool HeadAim() { return g_values[kHeadAim].load(std::memory_order_relaxed) != 0.0f; }
float TurnSpeedDeg() { return g_values[kTurnSpeedDeg].load(std::memory_order_relaxed); }
float StickFullDeflection() { return g_values[kStickFullDeflection].load(std::memory_order_relaxed); }
float TurnSpeed() { return g_values[kTurnSpeed].load(std::memory_order_relaxed); }

bool GunOnRightController() { return g_values[kGunOnRightController].load(std::memory_order_relaxed) != 0.0f; }
bool ShotsFromGun() { return g_values[kShotsFromGun].load(std::memory_order_relaxed) != 0.0f; }
bool LaserSight() { return g_values[kLaserSight].load(std::memory_order_relaxed) != 0.0f; }
bool TwoHandedAim() { return g_values[kTwoHandedAim].load(std::memory_order_relaxed) != 0.0f; }
void TwoHandParams(float* grabRadius, float* reach, float grip[2])
{
    // The settings are centimetres.
    *grabRadius = g_values[kTwoHandGrabRadius].load(std::memory_order_relaxed) * 0.01f;
    *reach = g_values[kTwoHandReach].load(std::memory_order_relaxed) * 0.01f;
    grip[0] = g_values[kSecondaryGripRight].load(std::memory_order_relaxed) * 0.01f;
    grip[1] = g_values[kSecondaryGripUp].load(std::memory_order_relaxed) * 0.01f;
}
void ShotOffset(float off[3], float angles[2])
{
    for (int i = 0; i < 3; ++i)
        off[i] = g_values[kShotOriginRight + i].load(std::memory_order_relaxed) * 0.01f; // the settings are centimetres
    angles[0] = g_values[kShotPitch].load(std::memory_order_relaxed);
    angles[1] = g_values[kShotYaw].load(std::memory_order_relaxed);
}
// Six consecutive values (right, up, forward, pitch, yaw, roll) from index first.
void SixFrom(int first, float off[3], float angles[3])
{
    for (int i = 0; i < 3; ++i)
    {
        off[i] = g_values[first + i].load(std::memory_order_relaxed);
        angles[i] = g_values[first + 3 + i].load(std::memory_order_relaxed);
    }
}
void GunOffset(float off[3], float angles[3]) { SixFrom(kGunOffsetRight, off, angles); }
void HandOffset(int hand, float off[3], float angles[3])
{
    SixFrom(hand ? kRightHandRight : kLeftHandRight, off, angles);
    for (int i = 0; i < 3; ++i)
        off[i] *= 0.01f; // the settings are centimetres
}
bool ArmsFollowControllers() { return g_values[kArmsFollowControllers].load(std::memory_order_relaxed) != 0.0f; }
void BodyOffset(float* forward, float* up)
{
    *forward = g_values[kBodyForward].load(std::memory_order_relaxed);
    *up = g_values[kBodyUp].load(std::memory_order_relaxed);
}
float TorsoTurnDeg() { return g_values[kStomachTurn].load(std::memory_order_relaxed); }
float ElbowSwivelDeg() { return g_values[kElbowSwivel].load(std::memory_order_relaxed); }
int HideBodyBonesFirst() { return static_cast<int>(g_values[kHideBodyBonesFirst].load(std::memory_order_relaxed)); }
int HideBodyBonesLast() { return static_cast<int>(g_values[kHideBodyBonesLast].load(std::memory_order_relaxed)); }
bool DebugMeshProbe() { return g_values[kDebugMeshProbe].load(std::memory_order_relaxed) != 0.0f; }
int DebugHidePass() { return static_cast<int>(g_values[kDebugHidePass].load(std::memory_order_relaxed)) - 1; }
bool HideDuplicateTracker() { return g_values[kHideDuplicateTracker].load(std::memory_order_relaxed) != 0.0f; }
int DebugHideDraw() { return static_cast<int>(g_values[kDebugHideDraw].load(std::memory_order_relaxed)); }

int MenuCount() { return kCount; }

bool GetMenu(int index, MenuInfo* out)
{
    if (index < 0 || index >= kCount || !out)
        return false;
    const Entry& e = kEntries[index];
    const float v = g_values[index].load(std::memory_order_relaxed);
    out->group = e.group;
    out->label = e.label;
    out->isToggle = e.kind == Kind::Toggle;
    out->isChoice = e.kind == Kind::Choice;
    out->value = v;
    out->decimals = e.decimals;
    out->unit = e.unit;
    out->text = e.kind == Kind::Choice && e.choiceLabel ? e.choiceLabel(static_cast<int>(v)) : "";
    return true;
}

void AdjustMenu(int index, int steps)
{
    if (index < 0 || index >= kCount || steps == 0)
        return;
    const Entry& e = kEntries[index];
    float v = g_values[index].load(std::memory_order_relaxed);
    if (e.kind == Kind::Toggle)
        v = v != 0.0f ? 0.0f : 1.0f;
    else if (e.kind == Kind::Choice)
    {
        const int n = static_cast<int>(e.maxValue) + 1;
        v = static_cast<float>(((static_cast<int>(v) + (steps > 0 ? 1 : -1)) % n + n) % n);
    }
    else
    {
        v += e.step * static_cast<float>(steps);
        v = std::round(v / e.step) * e.step; // stay on the step grid
        v = Clamp(e, v);
    }
    g_values[index].store(v, std::memory_order_relaxed);
    AVP_LOG(kLog, "%s -> %.3f", e.key, v);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteFileLocked();
}

} // namespace avp::settings
