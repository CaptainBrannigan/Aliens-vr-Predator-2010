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
#include <vector>

namespace avp::settings {
namespace {

constexpr const char* kLog = "avpvr_settings.log";
constexpr const char* kFileName = "avpvr_settings.ini";

enum class Kind
{
    Toggle,
    Number,
    Choice,
    Info, // read-only text (GetMenu's text), not saved
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
    {"GameCameraBase", "View", "Keep the game camera's motion (shake, recoil)", Kind::Toggle, 0.0f, 0, 0, 0, 0, "",
     nullptr},
    {"RollFromHeadOnly", "View", "Camera roll from your head only", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"HeadPosition", "View", "Head position (lean, crouch, step)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"RoomScaleStep", "View", "Room-scale: the character steps under your head", Kind::Toggle, 1.0f, 0, 0, 0, 0, "",
     nullptr},
    {"RoomScaleStepCm", "View", "Room-scale: step once your head is this far off", Kind::Number, 10.0f, 10.0f, 100.0f,
     1.0f, 0, "cm", nullptr},
    {"RoomScaleStepSpeed", "View", "Room-scale: step speed (stick)", Kind::Number, 0.5f, 0.1f, 0.5f, 0.05f, 2, "",
     nullptr},
    {"EyeFovBetweenFrames", "View", "Wide culling between frames (less pop-in)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "",
     nullptr},
    {"ScreenWarp", "View", "Screen warp effects (the Alien's fisheye)", Kind::Toggle, 0.0f, 0, 0, 0, 0, "", nullptr},
    {"HudPanel", "HUD", "HUD in a panel (off = full view)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"HudWidthDeg", "HUD", "HUD size (width)", Kind::Number, 88.0f, 20.0f, 110.0f, 1.0f, 0, "deg", nullptr},
    {"HudDistanceCm", "HUD", "HUD distance", Kind::Number, 50.0f, 1.0f, 2000.0f, 1.0f, 0, "cm", nullptr},
    {"HudHeightDeg", "HUD", "HUD height (+ up)", Kind::Number, -20.0f, -30.0f, 30.0f, 0.5f, 1, "deg", nullptr},
    {"HideDuplicateTracker", "HUD", "Hide the duplicate motion tracker", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"HideMarineReticle", "HUD", "Hide the reticle (Marine)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"TurnSpeedDeg", "Controls", "Turn speed (right stick)", Kind::Number, 150.0f, 30.0f, 360.0f, 5.0f, 0, "deg/s",
     nullptr},
    {"StickFullDeflection", "Controls", "Stick counts as full at", Kind::Number, 0.86f, 0.5f, 1.0f, 0.01f, 2, "",
     nullptr},
    {"TurnSpeed", "Controls", "Game look speed (head aim off)", Kind::Number, 1.0f, 0.2f, 3.0f, 0.05f, 2, "x",
     nullptr},
    {"HeadAim", "Head look", "Head aim (your head aims)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"LookOffsetUp", "Head look", "Look up from the view", Kind::Number, -12.5f, -15.0f, 15.0f, 0.25f, 2,
     "deg", nullptr},
    {"LookOffsetRight", "Head look", "Look right from the view", Kind::Number, 0.0f, -15.0f, 15.0f, 0.25f,
     2, "deg", nullptr},
    {"ArmsFollowControllers", "Hands", "Arms follow the controllers (Marine, Predator)", Kind::Toggle, 1.0f, 0, 0, 0,
     0, "", nullptr},
    {"SpeciesProfile", "Hands", "Body and hand values below are for", Kind::Info, 0, 0, 0, 0, 0, "", nullptr},
    {"BodyForward", "Hands", "Body (shoulders) forward", Kind::Number, 0.10f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"BodyUp", "Hands", "Body (shoulders) up", Kind::Number, -0.13f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"StomachTurn", "Hands", "Torso turn from the stomach (+ right)", Kind::Number, -15.0f, -90.0f, 90.0f, 1.0f, 0,
     "deg", nullptr},
    {"ElbowSwivel", "Hands", "Elbow swivel (+ lowers the elbows)", Kind::Number, 50.0f, -90.0f, 90.0f, 1.0f, 0, "deg",
     nullptr},
    {"LayerAnimation", "Hands", "Hands also move with the game's animation", Kind::Toggle, 1.0f, 0, 0, 0, 0, "",
     nullptr},
    {"GunOnRightController", "Weapon", "Gun on the right controller (Marine)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "",
     nullptr},
    {"WeaponProfile", "Weapon", "Gun and shot values below are for", Kind::Info, 0, 0, 0, 0, 0, "", nullptr},
    {"GunOffsetRight", "Weapon", "Gun offset right", Kind::Number, 0.0f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"GunOffsetUp", "Weapon", "Gun offset up", Kind::Number, 0.0f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"GunOffsetForward", "Weapon", "Gun offset forward", Kind::Number, 0.0f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"GunPitch", "Weapon", "Gun pitch (+ up)", Kind::Number, -60.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"GunYaw", "Weapon", "Gun yaw (+ right)", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"GunRoll", "Weapon", "Gun roll", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"TwoHandedAim", "Weapon", "Two-handed aim (left grip on the gun)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"TwoHandGrabRadius", "Weapon", "Two-handed: grab distance from the barrel", Kind::Number, 15.0f, 5.0f, 40.0f, 1.0f,
     0, "cm", nullptr},
    {"TwoHandReach", "Weapon", "Two-handed: reach along the barrel", Kind::Number, 80.0f, 20.0f, 120.0f, 1.0f, 0, "cm",
     nullptr},
    {"SecondaryGripRight", "Weapon", "Two-handed: off-hand grip right", Kind::Number, 0.0f, -20.0f, 20.0f, 0.5f, 1, "cm",
     nullptr},
    {"SecondaryGripUp", "Weapon", "Two-handed: off-hand grip up", Kind::Number, 0.0f, -20.0f, 20.0f, 0.5f, 1, "cm",
     nullptr},
    {"ShotsFromGun", "Aiming", "Shots from the gun (Marine)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"LaserSight", "Aiming", "Laser along the shots", Kind::Toggle, 0.0f, 0, 0, 0, 0, "", nullptr},
    {"ShotOriginRight", "Aiming", "Shot origin right", Kind::Number, -0.7f, -30.0f, 30.0f, 0.1f, 1, "cm", nullptr},
    {"ShotOriginUp", "Aiming", "Shot origin up", Kind::Number, 11.0f, -30.0f, 30.0f, 0.1f, 1, "cm", nullptr},
    {"ShotOriginForward", "Aiming", "Shot origin forward", Kind::Number, 12.0f, -30.0f, 60.0f, 0.1f, 1, "cm", nullptr},
    {"ShotPitch", "Aiming", "Shot pitch (+ up)", Kind::Number, 3.5f, -30.0f, 30.0f, 0.25f, 2, "deg", nullptr},
    {"ShotYaw", "Aiming", "Shot yaw (+ right)", Kind::Number, -1.75f, -30.0f, 30.0f, 0.25f, 2, "deg", nullptr},
    {"HandRight", "Hand placement", "Hand offset outward (right hand: right)", Kind::Number, 0.0f, -50.0f, 50.0f, 0.1f,
     1, "cm", nullptr},
    {"HandUp", "Hand placement", "Hand offset up", Kind::Number, -1.3f, -50.0f, 50.0f, 0.1f, 1, "cm", nullptr},
    {"HandForward", "Hand placement", "Hand offset forward", Kind::Number, -1.5f, -50.0f, 50.0f, 0.1f, 1, "cm", nullptr},
    {"HandPitch", "Hand placement", "Hand pitch (+ up)", Kind::Number, -58.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"HandYaw", "Hand placement", "Hand yaw (+ outward)", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"HandRoll", "Hand placement", "Hand roll (+ outward)", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg",
     nullptr},
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
    kHeadPosition,
    kRoomScaleStep,
    kRoomScaleStepCm,
    kRoomScaleStepSpeed,
    kEyeFovBetweenFrames,
    kScreenWarp,
    kHudPanel,
    kHudWidthDeg,
    kHudDistanceCm,
    kHudHeightDeg,
    kHideDuplicateTracker,
    kHideMarineReticle,
    kTurnSpeedDeg,
    kStickFullDeflection,
    kTurnSpeed,
    kHeadAim,
    kLookOffsetUp,
    kLookOffsetRight,
    kArmsFollowControllers,
    kSpeciesProfile,
    kBodyForward,
    kBodyUp,
    kStomachTurn,
    kElbowSwivel,
    kLayerAnimation,
    kGunOnRightController,
    kWeaponProfile,
    kGunOffsetRight,
    kGunOffsetUp,
    kGunOffsetForward,
    kGunPitch,
    kGunYaw,
    kGunRoll,
    kTwoHandedAim,
    kTwoHandGrabRadius,
    kTwoHandReach,
    kSecondaryGripRight,
    kSecondaryGripUp,
    kShotsFromGun,
    kLaserSight,
    kShotOriginRight,
    kShotOriginUp,
    kShotOriginForward,
    kShotPitch,
    kShotYaw,
    kHandRight,
    kHandUp,
    kHandForward,
    kHandPitch,
    kHandYaw,
    kHandRoll,
    kHideBodyBonesFirst,
    kHideBodyBonesLast,
    kDebugMeshProbe,
    kDebugHidePass,
    kDebugHideDraw,
};

std::atomic<float> g_values[kCount];
std::mutex g_fileMutex;

// ---- Profiles: groups of entries whose values depend on what's being played -- the gun's placement and shot line
// per weapon, the body and IK per species. The entries hold the CURRENT profile's values (what the dashboard shows and
// adjusts); switching puts them back into the previous profile and brings in the new one's (one seen for the first
// time starts from the defaults). Saved as "<current>=<key>" and "<prefix>.<key>.<setting>=<value>" lines. All under
// g_fileMutex.
struct Profile
{
    std::string key;
    std::vector<float> v;
};
struct ProfileSet
{
    const char* prefix;     // ini: <prefix>.<key>.<setting>
    const char* currentKey; // ini: <currentKey>=<key>
    std::vector<int> slots; // the entries it holds
    std::vector<Profile> profiles;
    std::string key;        // whose values the entries hold ("" = none yet)
    char name[64];          // GetMenu's text for the set's Info row
    bool newFromCurrent;    // one seen the first time starts from the current values (else the defaults)
};
ProfileSet g_weaponSet{"Weapon", "CurrentWeapon",
                       {kGunOffsetRight, kGunOffsetUp, kGunOffsetForward, kGunPitch, kGunYaw, kGunRoll,
                        kShotOriginRight, kShotOriginUp, kShotOriginForward, kShotPitch, kShotYaw, kTwoHandedAim,
                        kTwoHandGrabRadius, kTwoHandReach, kSecondaryGripRight, kSecondaryGripUp},
                       {}, "", "no weapon seen yet", false};
// Every other setting is per species (InitSets): all but the Info rows, the Debug group and the weapon set's.
ProfileSet g_speciesSet{"Species", "CurrentSpecies", {}, {}, "", "no species seen yet", false};
ProfileSet* const g_sets[] = {&g_weaponSet, &g_speciesSet};

Profile* FindProfile(ProfileSet& s, const std::string& key)
{
    for (Profile& p : s.profiles)
        if (p.key == key)
            return &p;
    return nullptr;
}

// The classes' own built-in defaults where they differ from the entries' (the Marine's): the user's tuned values,
// 2026-10-18. A class seen for the first time starts from these.
struct SpeciesDefault
{
    const char* species;
    int index;
    float value;
};
constexpr SpeciesDefault kSpeciesDefaults[] = {
    {"Predator", kRoomScaleStepCm, 30.0f},  {"Predator", kRoomScaleStepSpeed, 0.3f},
    {"Predator", kEyeFovBetweenFrames, 0.0f}, {"Predator", kHudDistanceCm, 30.0f},
    {"Predator", kHudHeightDeg, -10.0f},    {"Predator", kLookOffsetUp, -13.25f},
    {"Predator", kBodyForward, 0.09f},      {"Predator", kBodyUp, -0.01f},
    {"Predator", kStomachTurn, 0.0f},       {"Predator", kElbowSwivel, -20.0f},
    {"Predator", kHandForward, 0.5f},       {"Predator", kHandPitch, -86.0f},
    {"Predator", kHandYaw, 25.0f},
    {"Alien", kRoomScaleStepCm, 30.0f},     {"Alien", kRoomScaleStepSpeed, 0.3f},
    {"Alien", kEyeFovBetweenFrames, 0.0f},  {"Alien", kHudWidthDeg, 78.0f},
    {"Alien", kHudDistanceCm, 150.0f},      {"Alien", kHudHeightDeg, -15.0f},
    {"Alien", kLookOffsetUp, 0.0f},         {"Alien", kBodyForward, 0.13f},
    {"Alien", kBodyUp, -0.03f},             {"Alien", kStomachTurn, 0.0f},
    {"Alien", kElbowSwivel, 0.0f},
};

Profile& ProfileFor(ProfileSet& s, const std::string& key)
{
    if (Profile* p = FindProfile(s, key))
        return *p;
    Profile p;
    p.key = key;
    for (int slot : s.slots)
    {
        float v = kEntries[slot].defaultValue;
        if (&s == &g_speciesSet)
            for (const SpeciesDefault& d : kSpeciesDefaults)
                if (d.index == slot && key == d.species)
                    v = d.value;
        p.v.push_back(v);
    }
    s.profiles.push_back(p);
    return s.profiles.back();
}

// While loading: a profile from the file, its settings not (yet) read left NaN (filled from the main values).
Profile& ProfileForLoad(ProfileSet& s, const std::string& key)
{
    if (Profile* p = FindProfile(s, key))
        return *p;
    Profile p;
    p.key = key;
    p.v.assign(s.slots.size(), std::nanf(""));
    s.profiles.push_back(p);
    return s.profiles.back();
}

// The entries' values into the set's current profile.
void StoreCurrentLocked(ProfileSet& s)
{
    if (s.key.empty())
        return;
    Profile& p = ProfileFor(s, s.key);
    for (size_t j = 0; j < s.slots.size(); ++j)
        p.v[j] = g_values[s.slots[j]].load(std::memory_order_relaxed);
}

int SlotIn(const ProfileSet& s, const char* key)
{
    for (size_t j = 0; j < s.slots.size(); ++j)
        if (std::strcmp(kEntries[s.slots[j]].key, key) == 0)
            return static_cast<int>(j);
    return -1;
}

void SwitchProfileLocked(ProfileSet& s, const char* key, const char* name);

// The species set holds every entry the weapon set doesn't, except the Info rows and the Debug group.
void InitSets()
{
    if (!g_speciesSet.slots.empty())
        return;
    for (int i = 0; i < kCount; ++i)
    {
        bool weapon = false;
        for (int w : g_weaponSet.slots)
            weapon = weapon || w == i;
        if (!weapon && kEntries[i].kind != Kind::Info && std::strcmp(kEntries[i].group, "Debug") != 0)
            g_speciesSet.slots.push_back(i);
    }
}

// The species playing, for hiding settings it doesn't use: -1 unknown (show all), else g_PlayerSpecies (0 Predator,
// 1 Alien, 2 Marine).
std::atomic<int> g_speciesIndex{-1};

bool VisibleFor(int index)
{
    const int sp = g_speciesIndex.load(std::memory_order_relaxed);
    const bool marine = sp < 0 || sp == 2, predator = sp < 0 || sp == 0;
    const char* group = kEntries[index].group;
    if (index == kHideDuplicateTracker || index == kHideMarineReticle || index == kStomachTurn)
        return marine; // the motion tracker, the reticle hide, the Marine's torso bones
    if (std::strcmp(group, "Weapon") == 0 || std::strcmp(group, "Aiming") == 0)
        return marine; // the guns
    if (std::strcmp(group, "Hands") == 0 || std::strcmp(group, "Hand placement") == 0)
        return marine || predator; // the arm rigs
    if (std::strcmp(group, "Head look") == 0 && index != kHeadAim)
        return g_values[kHeadAim].load(std::memory_order_relaxed) != 0.0f;
    if (index == kRoomScaleStep || index == kRoomScaleStepCm || index == kRoomScaleStepSpeed)
        return g_values[kHeadPosition].load(std::memory_order_relaxed) != 0.0f &&
               (index == kRoomScaleStep || g_values[kRoomScaleStep].load(std::memory_order_relaxed) != 0.0f);
    return true;
}

// Old keys (before the per-class settings, 2026-10-18): the right hand's offsets became the shared hand's (the left
// hand's are dropped), and each species' aim offset its own look offset.
const char* RenamedKey(const std::string& key)
{
    static const char* const kRenames[][2] = {
        {"RightHandRight", "HandRight"}, {"RightHandUp", "HandUp"},   {"RightHandForward", "HandForward"},
        {"RightHandPitch", "HandPitch"}, {"RightHandYaw", "HandYaw"}, {"RightHandRoll", "HandRoll"},
    };
    for (const auto& r : kRenames)
        if (key == r[0])
            return r[1];
    return nullptr;
}
bool OldAimKey(const std::string& key, std::string* species, const char** newKey)
{
    for (const char* s : {"Marine", "Alien", "Predator"})
        for (int up = 0; up < 2; ++up)
            if (key == std::string(s) + (up ? "AimUp" : "AimRight"))
            {
                *species = s;
                *newKey = up ? "LookOffsetUp" : "LookOffsetRight";
                return true;
            }
    return false;
}

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
    char line[200];
    for (int i = 0; i < kCount; ++i)
    {
        const Entry& e = kEntries[i];
        const float v = g_values[i].load(std::memory_order_relaxed);
        if (e.kind == Kind::Info)
            continue;
        if (e.kind == Kind::Toggle)
            snprintf(line, sizeof(line), "%s=%d\r\n", e.key, v != 0.0f ? 1 : 0);
        else if (e.kind == Kind::Choice)
            snprintf(line, sizeof(line), "%s=%d\r\n", e.key, static_cast<int>(v));
        else
            snprintf(line, sizeof(line), "%s=%.*f\r\n", e.key, e.decimals + 2, v);
        text += line;
    }
    // Profiles (the current ones from the entries).
    for (ProfileSet* s : g_sets)
    {
        StoreCurrentLocked(*s);
        if (!s->key.empty())
        {
            snprintf(line, sizeof(line), "; per-%s values; the ones above are %s's\r\n%s=%s\r\n", s->prefix,
                     s->currentKey, s->currentKey, s->key.c_str());
            text += line;
        }
        for (const Profile& p : s->profiles)
            for (size_t j = 0; j < s->slots.size(); ++j)
            {
                const Entry& e = kEntries[s->slots[j]];
                snprintf(line, sizeof(line), "%s.%s.%s=%.*f\r\n", s->prefix, p.key.c_str(), e.key, e.decimals + 2,
                         p.v[j]);
                text += line;
            }
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
    InitSets();
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
    struct OldAim
    {
        std::string species;
        const char* key;
        float value;
    };
    std::vector<OldAim> oldAims;
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
        std::string key = line.substr(0, eq);
        {
            // Renamed settings, in main keys and in profile lines (<prefix>.<key>.<setting>).
            const size_t lastDot = key.rfind('.');
            const std::string setting = lastDot == std::string::npos ? key : key.substr(lastDot + 1);
            if (const char* renamed = RenamedKey(setting))
                key = (lastDot == std::string::npos ? std::string() : key.substr(0, lastDot + 1)) + renamed;
            if (setting == "HudDistance") // metres until 2026-10-18, now HudDistanceCm
            {
                key = (lastDot == std::string::npos ? std::string() : key.substr(0, lastDot + 1)) + "HudDistanceCm";
                char scaled[32];
                snprintf(scaled, sizeof(scaled), "%.1f", atof(line.c_str() + eq + 1) * 100.0);
                line = line.substr(0, eq + 1) + scaled;
            }
        }
        std::string aimSpecies;
        const char* aimKey = nullptr;
        if (OldAimKey(key, &aimSpecies, &aimKey))
        {
            oldAims.push_back({aimSpecies, aimKey, static_cast<float>(atof(line.c_str() + eq + 1))});
            continue;
        }
        bool profileLine = false;
        for (ProfileSet* s : g_sets)
        {
            const std::string prefix = std::string(s->prefix) + ".";
            if (key == s->currentKey)
            {
                s->key = line.substr(eq + 1);
                profileLine = true;
            }
            else if (key.compare(0, prefix.size(), prefix) == 0)
            {
                // <prefix>.<key>.<setting>
                const size_t dot = key.find('.', prefix.size());
                const int slot = dot == std::string::npos ? -1 : SlotIn(*s, key.c_str() + dot + 1);
                if (slot >= 0)
                    ProfileForLoad(*s, key.substr(prefix.size(), dot - prefix.size())).v[slot] =
                        Clamp(kEntries[s->slots[slot]], static_cast<float>(atof(line.c_str() + eq + 1)));
                profileLine = true;
            }
        }
        if (profileLine)
            continue;
        for (int i = 0; i < kCount; ++i)
            if (key == kEntries[i].key && kEntries[i].kind != Kind::Info)
            {
                g_values[i].store(Clamp(kEntries[i], static_cast<float>(atof(line.c_str() + eq + 1))),
                                  std::memory_order_relaxed);
                ++loaded;
            }
    }
    // The old per-species aim offsets: each into its species' look offset (the entries' own for the current species).
    for (const OldAim& a : oldAims)
    {
        const int index = SlotIn(g_speciesSet, a.key);
        if (index < 0)
            continue;
        const float v = Clamp(kEntries[g_speciesSet.slots[index]], a.value);
        if (a.species == g_speciesSet.key)
            g_values[g_speciesSet.slots[index]].store(v, std::memory_order_relaxed);
        else
            ProfileForLoad(g_speciesSet, a.species).v[index] = v;
    }
    // Profile settings the file didn't have (new settings, profiles saved before every setting was per class):
    // the values above.
    for (ProfileSet* s : g_sets)
        for (Profile& p : s->profiles)
            for (size_t j = 0; j < s->slots.size(); ++j)
                if (std::isnan(p.v[j]))
                    p.v[j] = g_values[s->slots[j]].load(std::memory_order_relaxed);
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
float HudDistance() { return g_values[kHudDistanceCm].load(std::memory_order_relaxed) * 0.01f; } // the setting is cm
float HudHeightDeg() { return g_values[kHudHeightDeg].load(std::memory_order_relaxed); }

bool GameCameraBase() { return g_values[kGameCameraBase].load(std::memory_order_relaxed) != 0.0f; }
bool HeadPosition() { return g_values[kHeadPosition].load(std::memory_order_relaxed) != 0.0f; }
bool RoomScaleStep() { return g_values[kRoomScaleStep].load(std::memory_order_relaxed) != 0.0f; }
float RoomScaleStepDistance() { return g_values[kRoomScaleStepCm].load(std::memory_order_relaxed) * 0.01f; }
float RoomScaleStepSpeed() { return g_values[kRoomScaleStepSpeed].load(std::memory_order_relaxed); }
bool LayerAnimation() { return g_values[kLayerAnimation].load(std::memory_order_relaxed) != 0.0f; }
bool EyeFovBetweenFrames() { return g_values[kEyeFovBetweenFrames].load(std::memory_order_relaxed) != 0.0f; }
bool ScreenWarp() { return g_values[kScreenWarp].load(std::memory_order_relaxed) != 0.0f; }
bool RollFromHeadOnly() { return g_values[kRollFromHeadOnly].load(std::memory_order_relaxed) != 0.0f; }
bool HideMarineReticle() { return g_values[kHideMarineReticle].load(std::memory_order_relaxed) != 0.0f; }

void LookOffsetDeg(float* up, float* right)
{
    *up = g_values[kLookOffsetUp].load(std::memory_order_relaxed);
    *right = g_values[kLookOffsetRight].load(std::memory_order_relaxed);
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
    // One setting for both hands, mirrored for the left: outward = right for the right hand, left for the left, and
    // yaw / roll outward likewise.
    SixFrom(kHandRight, off, angles);
    for (int i = 0; i < 3; ++i)
        off[i] *= 0.01f; // the settings are centimetres
    if (hand == 0)
    {
        off[0] = -off[0];
        angles[1] = -angles[1];
        angles[2] = -angles[2];
    }
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

namespace {
void SwitchProfileLocked(ProfileSet& s, const char* key, const char* name)
{
    snprintf(s.name, sizeof(s.name), "%s", name);
    if (s.key == key)
        return;
    StoreCurrentLocked(s); // the previous profile's values kept
    const bool hadOne = !s.key.empty();
    const bool known = FindProfile(s, key) != nullptr;
    s.key = key;
    // A class: its own built-in defaults even as the first profile (kSpeciesDefaults).
    const bool fresh = (hadOne || &s == &g_speciesSet) && !s.newFromCurrent;
    if (known || fresh)
    {
        // Its own values, or the defaults the first time.
        const Profile& p = ProfileFor(s, s.key);
        for (size_t j = 0; j < s.slots.size(); ++j)
            g_values[s.slots[j]].store(p.v[j], std::memory_order_relaxed);
    }
    // (None before -- the first one after an update without these profiles -- keeps the values as they are.)
    AVP_LOG(kLog, "%s profile: %s (%s), %s", s.prefix, key, name,
            known ? "its own values" : fresh ? "new: the defaults" : "new: the current values");
    WriteFileLocked();
}
} // namespace

void SetCurrentWeapon(const char* key, const char* name)
{
    std::lock_guard<std::mutex> lock(g_fileMutex);
    SwitchProfileLocked(g_weaponSet, key, name);
}

void SetCurrentSpecies(const char* key, const char* name)
{
    const int index = std::strcmp(key, "Predator") == 0 ? 0
                      : std::strcmp(key, "Alien") == 0  ? 1
                      : std::strcmp(key, "Marine") == 0 ? 2
                                                        : -1;
    g_speciesIndex.store(index, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    SwitchProfileLocked(g_speciesSet, key, name);
}

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
    out->isInfo = e.kind == Kind::Info;
    out->hidden = !VisibleFor(index);
    out->value = v;
    out->decimals = e.decimals;
    out->unit = e.unit;
    out->text = e.kind == Kind::Choice && e.choiceLabel ? e.choiceLabel(static_cast<int>(v))
                : index == kWeaponProfile               ? g_weaponSet.name
                : index == kSpeciesProfile              ? g_speciesSet.name
                                                        : "";
    return true;
}

void AdjustMenu(int index, int steps)
{
    if (index < 0 || index >= kCount || steps == 0)
        return;
    const Entry& e = kEntries[index];
    if (e.kind == Kind::Info)
        return;
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
