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

// What a holster spot presses on the pad.
struct HolsterAction
{
    const char* label;
    unsigned mask;
};
constexpr HolsterAction kHolsterActions[] = {
    {"nothing", 0},       {"D-pad up", 0x0001}, {"D-pad down", 0x0002}, {"D-pad left", 0x0004},
    {"D-pad right", 0x0008}, {"A", 0x1000},     {"B", 0x2000},          {"X", 0x4000},
    {"Y", 0x8000},        {"LB", 0x0100},       {"RB", 0x0200},         {"Left stick click", 0x0040},
    {"Right stick click", 0x0080}, {"Back", 0x0020}, {"Start", 0x0010},
};
constexpr int kHolsterActionCount = static_cast<int>(sizeof(kHolsterActions) / sizeof(kHolsterActions[0]));
const char* HolsterActionLabel(int i) { return kHolsterActions[i >= 0 && i < kHolsterActionCount ? i : 0].label; }

// Where the flashlight sits; the order is the setting's values (settings.h kFlashlight*).
constexpr const char* kFlashlightMounts[] = {"The game's spot", "Head", "Chest", "Right hand", "Left hand", "Weapon"};
constexpr int kFlashlightMountCount = static_cast<int>(sizeof(kFlashlightMounts) / sizeof(kFlashlightMounts[0]));
const char* FlashlightMountLabel(int i) { return kFlashlightMounts[i >= 0 && i < kFlashlightMountCount ? i : 0]; }

// Order = display order; entries of a group must be contiguous.
const Entry kEntries[] = {
    {"IpdScale", "View", "Eye distance (IPD) scale", Kind::Number, 1.0f, 0.5f, 2.0f, 0.02f, 2, "x", nullptr},
    {"GameCameraBase", "View", "Keep the game camera's motion (shake, recoil)", Kind::Toggle, 0.0f, 0, 0, 0, 0, "",
     nullptr},
    {"RollFromHeadOnly", "View", "Camera roll from your head only", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"HeadPosition", "View", "Head position (lean, crouch, step)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"RoomScaleStep", "View", "Room-scale: the character steps under your head", Kind::Toggle, 0.0f, 0, 0, 0, 0, "",
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
    {"HudDistanceCm", "HUD", "HUD distance", Kind::Number, 20.0f, 1.0f, 2000.0f, 1.0f, 0, "cm", nullptr},
    {"HudHeightDeg", "HUD", "HUD height (+ up)", Kind::Number, -15.0f, -30.0f, 30.0f, 0.5f, 1, "deg", nullptr},
    {"HideDuplicateTracker", "HUD", "Hide the duplicate motion tracker", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"HideMarineReticle", "HUD", "Hide the reticle (Marine)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"TurnSpeedDeg", "Controls", "Turn speed (right stick)", Kind::Number, 150.0f, 30.0f, 360.0f, 5.0f, 0, "deg/s",
     nullptr},
    {"StickFullDeflection", "Controls", "Stick counts as full at", Kind::Number, 0.86f, 0.5f, 1.0f, 0.01f, 2, "",
     nullptr},
    {"TurnSpeed", "Controls", "Game look speed (head aim off)", Kind::Number, 1.0f, 0.2f, 3.0f, 0.05f, 2, "x",
     nullptr},
    {"Rumble", "Controls", "Controller rumble (the game's)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"RumbleStrength", "Controls", "Rumble strength", Kind::Number, 1.0f, 0.1f, 1.0f, 0.05f, 2, "x", nullptr},
    {"HeadAim", "Head look", "Head aim (your head aims)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"LookOffsetUp", "Head look", "Look up from the view", Kind::Number, -12.5f, -15.0f, 15.0f, 0.25f, 2,
     "deg", nullptr},
    {"LookOffsetRight", "Head look", "Look right from the view", Kind::Number, 0.0f, -15.0f, 15.0f, 0.25f,
     2, "deg", nullptr},
    {"ArmsFollowControllers", "Hands", "Arms follow the controllers (Marine, Predator)", Kind::Toggle, 1.0f, 0, 0, 0,
     0, "", nullptr},
    {"LeftHanded", "Hands", "Left-handed (arms, gun, triggers, grips)", Kind::Toggle, 0.0f, 0, 0, 0, 0, "", nullptr},
    {"SpeciesProfile", "Hands", "Body and hand values below are for", Kind::Info, 0, 0, 0, 0, 0, "", nullptr},
    {"BodyForward", "Hands", "Body (shoulders) forward", Kind::Number, 0.06f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"BodyUp", "Hands", "Body (shoulders) up", Kind::Number, -0.13f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"StomachTurn", "Hands", "Torso turn from the stomach (+ right)", Kind::Number, -15.0f, -90.0f, 90.0f, 1.0f, 0,
     "deg", nullptr},
    {"LeftShoulderOutward", "Hands", "Left shoulder outward", Kind::Number, 0.0f, -50.0f, 50.0f, 0.5f, 1, "cm", nullptr},
    {"LeftShoulderUp", "Hands", "Left shoulder up", Kind::Number, 0.0f, -50.0f, 50.0f, 0.5f, 1, "cm", nullptr},
    {"LeftShoulderForward", "Hands", "Left shoulder forward", Kind::Number, 0.0f, -50.0f, 50.0f, 0.5f, 1, "cm", nullptr},
    {"RightShoulderOutward", "Hands", "Right shoulder outward", Kind::Number, 0.0f, -50.0f, 50.0f, 0.5f, 1, "cm",
     nullptr},
    {"RightShoulderUp", "Hands", "Right shoulder up", Kind::Number, 0.0f, -50.0f, 50.0f, 0.5f, 1, "cm", nullptr},
    {"RightShoulderForward", "Hands", "Right shoulder forward", Kind::Number, 0.0f, -50.0f, 50.0f, 0.5f, 1, "cm",
     nullptr},
    {"ElbowSwivel", "Hands", "Elbow swivel (+ lowers the elbows)", Kind::Number, 50.0f, -90.0f, 90.0f, 1.0f, 0, "deg",
     nullptr},
    {"ElbowRollModel", "Hands", "Elbows follow the wrist's roll (natural model)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "",
     nullptr},
    {"ElbowFollowRoll", "Hands", "Elbow: how much it follows the wrist's roll", Kind::Number, 0.75f, 0.0f, 1.0f, 0.05f,
     2, "", nullptr},
    {"ElbowInKnee", "Hands", "Elbow inward: follows exactly up to", Kind::Number, 10.0f, 0.0f, 150.0f, 1.0f, 0, "deg",
     nullptr},
    {"ElbowInMax", "Hands", "Elbow inward: at most", Kind::Number, 25.0f, 0.0f, 170.0f, 1.0f, 0, "deg", nullptr},
    {"ElbowInNearBody", "Hands", "Elbow inward with the hand near the body", Kind::Number, 0.5f, 0.0f, 1.0f, 0.05f, 2,
     "x", nullptr},
    {"ElbowOutKnee", "Hands", "Elbow outward: follows exactly up to", Kind::Number, 100.0f, 0.0f, 150.0f, 1.0f, 0,
     "deg", nullptr},
    {"ElbowOutMax", "Hands", "Elbow outward: at most", Kind::Number, 130.0f, 0.0f, 170.0f, 1.0f, 0, "deg", nullptr},
    {"ElbowSmoothingMs", "Hands", "Elbow smoothing", Kind::Number, 50.0f, 0.0f, 300.0f, 10.0f, 0, "ms", nullptr},
    {"LayerAnimation", "Hands", "Hands also move with the game's animation", Kind::Toggle, 0.0f, 0, 0, 0, 0, "",
     nullptr},
    {"GunOnRightController", "Weapon", "Gun on the controller (Marine)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "",
     nullptr},
    {"WeaponProfile", "Weapon", "Gun and shot values below are for", Kind::Info, 0, 0, 0, 0, 0, "", nullptr},
    {"GunOffsetRight", "Weapon", "Gun offset right", Kind::Number, 0.0f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"GunOffsetUp", "Weapon", "Gun offset up", Kind::Number, 0.0f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"GunOffsetForward", "Weapon", "Gun offset forward", Kind::Number, 0.0f, -0.5f, 0.5f, 0.01f, 2, "m", nullptr},
    {"GunPitch", "Weapon", "Gun pitch (+ up)", Kind::Number, -60.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"GunYaw", "Weapon", "Gun yaw (+ right)", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"GunRoll", "Weapon", "Gun roll", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"TwoHandedAim", "Weapon", "Two-handed aim (off-hand grip on the gun)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
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
    {"MuzzleFlashOnGun", "Aiming", "Muzzle flash on the gun (Marine)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"MuzzleFlashRight", "Aiming", "Muzzle flash right of the shot origin", Kind::Number, 0.0f, -30.0f, 30.0f, 0.1f, 1,
     "cm", nullptr},
    {"MuzzleFlashUp", "Aiming", "Muzzle flash up", Kind::Number, 0.0f, -30.0f, 30.0f, 0.1f, 1, "cm", nullptr},
    {"MuzzleFlashForward", "Aiming", "Muzzle flash forward", Kind::Number, 0.0f, -30.0f, 60.0f, 0.1f, 1, "cm", nullptr},
    {"ShotOriginRight", "Aiming", "Shot origin right", Kind::Number, -0.7f, -30.0f, 30.0f, 0.1f, 1, "cm", nullptr},
    {"ShotOriginUp", "Aiming", "Shot origin up", Kind::Number, 11.0f, -30.0f, 30.0f, 0.1f, 1, "cm", nullptr},
    {"ShotOriginForward", "Aiming", "Shot origin forward", Kind::Number, 12.0f, -30.0f, 60.0f, 0.1f, 1, "cm", nullptr},
    {"ShotPitch", "Aiming", "Shot pitch (+ up)", Kind::Number, 3.5f, -30.0f, 30.0f, 0.25f, 2, "deg", nullptr},
    {"ShotYaw", "Aiming", "Shot yaw (+ right)", Kind::Number, -1.75f, -30.0f, 30.0f, 0.25f, 2, "deg", nullptr},
    {"FlashlightMount", "Flashlight", "Flashlight on", Kind::Choice, static_cast<float>(kFlashlightGame), 0.0f,
     static_cast<float>(kFlashlightMountCount - 1), 1.0f, 0, "", &FlashlightMountLabel},
    {"FlashlightRight", "Flashlight", "Flashlight offset right", Kind::Number, 0.0f, -50.0f, 50.0f, 0.5f, 1, "cm",
     nullptr},
    {"FlashlightUp", "Flashlight", "Flashlight offset up", Kind::Number, 0.0f, -50.0f, 50.0f, 0.5f, 1, "cm", nullptr},
    {"FlashlightForward", "Flashlight", "Flashlight offset forward", Kind::Number, 0.0f, -50.0f, 100.0f, 0.5f, 1, "cm",
     nullptr},
    {"HandRight", "Hand placement", "Hand offset outward (right hand: right)", Kind::Number, 0.0f, -50.0f, 50.0f, 0.1f,
     1, "cm", nullptr},
    {"HandUp", "Hand placement", "Hand offset up", Kind::Number, -1.3f, -50.0f, 50.0f, 0.1f, 1, "cm", nullptr},
    {"HandForward", "Hand placement", "Hand offset forward", Kind::Number, -1.5f, -50.0f, 50.0f, 0.1f, 1, "cm", nullptr},
    {"HandPitch", "Hand placement", "Hand pitch (+ up)", Kind::Number, -58.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"HandYaw", "Hand placement", "Hand yaw (+ outward)", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg", nullptr},
    {"HandRoll", "Hand placement", "Hand roll (+ outward)", Kind::Number, 0.0f, -180.0f, 180.0f, 1.0f, 0, "deg",
     nullptr},
    {"Holsters", "Holsters", "Holsters (grip in a body spot)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"HolsterGrabHand", "Holsters", "Weapon goes to the hand that grabs it (Marine)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "",
     nullptr},
    {"HolsterHoldToKeep", "Holsters", "Hold the grip to keep it in that hand", Kind::Toggle, 0.0f, 0, 0, 0, 0, "",
     nullptr},
    {"ShowGrabZones", "Holsters", "Show the grab zones (for setting them up)", Kind::Toggle, 0.0f, 0, 0, 0, 0, "",
     nullptr},
    {"HolsterRadiusCm", "Holsters", "Holster size", Kind::Number, 15.0f, 5.0f, 40.0f, 1.0f, 0, "cm", nullptr},
    {"HolsterDropCm", "Holsters", "Holster height offset (+ lower)", Kind::Number, 0.0f, -40.0f, 40.0f, 1.0f, 0, "cm",
     nullptr},
    {"HolsterStomachRight", "Holsters", "Stomach: to each side", Kind::Number, 10.0f, 0.0f, 60.0f, 1.0f, 0, "cm", nullptr},
    {"HolsterStomachUp", "Holsters", "Stomach: up from the head", Kind::Number, -45.0f, -120.0f, 30.0f, 1.0f, 0, "cm",
     nullptr},
    {"HolsterStomachForward", "Holsters", "Stomach: forward", Kind::Number, 12.0f, -50.0f, 60.0f, 1.0f, 0, "cm", nullptr},
    {"HolsterHipRight", "Holsters", "Hip: to each side", Kind::Number, 22.0f, 0.0f, 60.0f, 1.0f, 0, "cm", nullptr},
    {"HolsterHipUp", "Holsters", "Hip: up from the head", Kind::Number, -58.0f, -120.0f, 30.0f, 1.0f, 0, "cm", nullptr},
    {"HolsterHipForward", "Holsters", "Hip: forward", Kind::Number, 0.0f, -50.0f, 60.0f, 1.0f, 0, "cm", nullptr},
    {"HolsterChestRight", "Holsters", "Chest: right", Kind::Number, 0.0f, -60.0f, 60.0f, 1.0f, 0, "cm", nullptr},
    {"HolsterChestUp", "Holsters", "Chest: up from the head", Kind::Number, -23.0f, -120.0f, 30.0f, 1.0f, 0, "cm", nullptr},
    {"HolsterChestForward", "Holsters", "Chest: forward", Kind::Number, 14.0f, -50.0f, 60.0f, 1.0f, 0, "cm", nullptr},
    {"HolsterLeftShoulderRight", "Holsters", "Left shoulder: right", Kind::Number, -20.0f, -60.0f, 60.0f, 1.0f, 0, "cm",
     nullptr},
    {"HolsterLeftShoulderUp", "Holsters", "Left shoulder: up from the head", Kind::Number, -10.0f, -120.0f, 30.0f, 1.0f, 0,
     "cm", nullptr},
    {"HolsterLeftShoulderForward", "Holsters", "Left shoulder: forward", Kind::Number, -10.0f, -50.0f, 60.0f, 1.0f, 0,
     "cm", nullptr},
    {"HolsterRightShoulderRight", "Holsters", "Right shoulder: right", Kind::Number, 20.0f, -60.0f, 60.0f, 1.0f, 0, "cm",
     nullptr},
    {"HolsterRightShoulderUp", "Holsters", "Right shoulder: up from the head", Kind::Number, -10.0f, -120.0f, 30.0f, 1.0f,
     0, "cm", nullptr},
    {"HolsterRightShoulderForward", "Holsters", "Right shoulder: forward", Kind::Number, -10.0f, -50.0f, 60.0f, 1.0f, 0,
     "cm", nullptr},
    {"HolsterStomachAction", "Holsters", "Stomach presses", Kind::Choice, 3.0f, 0.0f,
     static_cast<float>(kHolsterActionCount - 1), 1.0f, 0, "", &HolsterActionLabel},
    {"HolsterHipAction", "Holsters", "Hip presses", Kind::Choice, 1.0f, 0.0f, static_cast<float>(kHolsterActionCount - 1),
     1.0f, 0, "", &HolsterActionLabel},
    {"HolsterChestAction", "Holsters", "Chest presses", Kind::Choice, 4.0f, 0.0f,
     static_cast<float>(kHolsterActionCount - 1), 1.0f, 0, "", &HolsterActionLabel},
    {"HolsterLeftShoulderAction", "Holsters", "Left shoulder presses", Kind::Choice, 3.0f, 0.0f,
     static_cast<float>(kHolsterActionCount - 1), 1.0f, 0, "", &HolsterActionLabel},
    {"HolsterRightShoulderAction", "Holsters", "Right shoulder presses", Kind::Choice, 3.0f, 0.0f,
     static_cast<float>(kHolsterActionCount - 1), 1.0f, 0, "", &HolsterActionLabel},
    {"HideBodyBonesFirst", "Debug", "Hide body bones from #", Kind::Number, 0.0f, 0.0f, 128.0f, 1.0f, 0, "", nullptr},
    {"HideBodyBonesLast", "Debug", "Hide body bones to #", Kind::Number, 0.0f, 0.0f, 128.0f, 1.0f, 0, "", nullptr},
    {"DebugMeshProbe", "Debug", "Mesh probe (log the body pass's draws)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"DebugElbowLog", "Debug", "Elbow model log (every frame)", Kind::Toggle, 0.0f, 0, 0, 0, 0, "", nullptr},
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
    kRumble,
    kRumbleStrength,
    kHeadAim,
    kLookOffsetUp,
    kLookOffsetRight,
    kArmsFollowControllers,
    kLeftHanded,
    kSpeciesProfile,
    kBodyForward,
    kBodyUp,
    kStomachTurn,
    kLeftShoulderOutward,
    kLeftShoulderUp,
    kLeftShoulderForward,
    kRightShoulderOutward,
    kRightShoulderUp,
    kRightShoulderForward,
    kElbowSwivel,
    kElbowRollModel,
    kElbowFollowRoll,
    kElbowInKnee,
    kElbowInMax,
    kElbowInNearBody,
    kElbowOutKnee,
    kElbowOutMax,
    kElbowSmoothingMs,
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
    kMuzzleFlashOnGun,
    kMuzzleFlashRight,
    kMuzzleFlashUp,
    kMuzzleFlashForward,
    kShotOriginRight,
    kShotOriginUp,
    kShotOriginForward,
    kShotPitch,
    kShotYaw,
    kFlashlightMount,
    kFlashlightRight,
    kFlashlightUp,
    kFlashlightForward,
    kHandRight,
    kHandUp,
    kHandForward,
    kHandPitch,
    kHandYaw,
    kHandRoll,
    kHolsters,
    kHolsterGrabHand,
    kHolsterHoldToKeep,
    kShowGrabZones,
    kHolsterRadiusCm,
    kHolsterDropCm,
    kHolsterPosFirst, // three each (right, up, forward), in settings::kHolster* order
    kHolsterActionFirst = kHolsterPosFirst + 3 * kHolsterZoneCount,
    kHolsterActionLast = kHolsterActionFirst + kHolsterZoneCount - 1,
    kHideBodyBonesFirst,
    kHideBodyBonesLast,
    kDebugMeshProbe,
    kDebugElbowLog,
    kDebugHidePass,
    kDebugHideDraw,
};

std::atomic<float> g_values[kCount];
std::mutex g_fileMutex;

// GAME CONTROLLERS: two dashboard rows per game controller product DirectInput lists to the game (HidePadDevice), after
// the table's rows: menu index kCount = the motion controllers' status, kCount + 1 + 2i = product i's status,
// kCount + 2 + 2i = its hide toggle. Rows are written once before g_padRowCount counts them (the dashboard thread reads
// the name unlocked); hide and state are the live values.
constexpr int kPadDeviceRows = 8;
struct PadDeviceRow
{
    char id[64];
    char name[160];
    std::atomic<bool> hide;
    std::atomic<int> state; // PadDeviceState
};
const char* PadDeviceStateText(int state)
{
    switch (state)
    {
    case kPadSent: return "input sent to the game";
    case kPadNotRead: return "not sent: another controller is read";
    case kPadHidden: return "not sent: hidden";
    default: return "not connected";
    }
}
PadDeviceRow g_padRows[kPadDeviceRows];
std::atomic<int> g_padRowCount{0};
// Every product's saved choice, by id (HidePadProduct.<VIDPID>=0/1), including ones not connected this launch.
// g_fileMutex.
std::vector<std::pair<std::string, bool>> g_padSaved;
std::atomic<bool> g_padChanged{false};
constexpr int kPadInfoRow = kCount;
constexpr int kMenuCount = kCount + 1 + 2 * kPadDeviceRows;

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
                        kTwoHandGrabRadius, kTwoHandReach, kSecondaryGripRight, kSecondaryGripUp, kMuzzleFlashRight,
                        kMuzzleFlashUp, kMuzzleFlashForward},
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

// Built-in profile defaults where they differ from the entries' (the Marine's): the user's tuned values (2026-10-18,
// updated for 1.0 on 2026-10-22). A class or weapon seen for the first time starts from these.
struct ProfileDefault
{
    const char* key; // the class's name / the weapon's mesh id
    int index;
    float value;
};
constexpr ProfileDefault kSpeciesDefaults[] = {
    {"Predator", kRoomScaleStepCm, 30.0f},    {"Predator", kRoomScaleStepSpeed, 0.3f},
    {"Predator", kEyeFovBetweenFrames, 0.0f}, {"Predator", kHudDistanceCm, 30.0f},
    {"Predator", kHudHeightDeg, -10.0f},      {"Predator", kLookOffsetUp, -13.25f},
    {"Predator", kBodyForward, 0.09f},        {"Predator", kBodyUp, -0.01f},
    {"Predator", kStomachTurn, 0.0f},         {"Predator", kElbowSwivel, -20.0f},
    {"Predator", kElbowFollowRoll, 0.5f},     {"Predator", kElbowInKnee, 30.0f},
    {"Predator", kElbowInMax, 60.0f},         {"Predator", kElbowInNearBody, 0.25f},
    {"Predator", kElbowOutKnee, 60.0f},       {"Predator", kElbowOutMax, 110.0f},
    {"Predator", kElbowSmoothingMs, 80.0f},   {"Predator", kLayerAnimation, 1.0f},
    {"Predator", kHandForward, 0.5f},         {"Predator", kHandPitch, -86.0f},
    {"Predator", kHandYaw, 25.0f},
    {"Alien", kRoomScaleStepCm, 30.0f},       {"Alien", kRoomScaleStepSpeed, 0.3f},
    {"Alien", kEyeFovBetweenFrames, 0.0f},    {"Alien", kHudWidthDeg, 78.0f},
    {"Alien", kHudDistanceCm, 150.0f},        {"Alien", kHudHeightDeg, -15.0f},
    {"Alien", kLookOffsetUp, 0.0f},           {"Alien", kBodyForward, 0.13f},
    {"Alien", kBodyUp, -0.03f},               {"Alien", kStomachTurn, 0.0f},
    {"Alien", kElbowSwivel, 0.0f},
    // The Marine's holsters pick weapons with the D-pad (the entries' defaults); the others' D-pads do something else.
    {"Predator", kHolsterActionFirst + 0, 0.0f}, {"Predator", kHolsterActionFirst + 1, 0.0f},
    {"Predator", kHolsterActionFirst + 2, 0.0f}, {"Predator", kHolsterActionFirst + 3, 0.0f},
    {"Predator", kHolsterActionFirst + 4, 0.0f}, {"Alien", kHolsterActionFirst + 0, 0.0f},
    {"Alien", kHolsterActionFirst + 1, 0.0f},    {"Alien", kHolsterActionFirst + 2, 0.0f},
    {"Alien", kHolsterActionFirst + 3, 0.0f},    {"Alien", kHolsterActionFirst + 4, 0.0f},
    // Their holster heights (the stomach, hip and chest: the entries' are the Marine's).
    {"Predator", kHolsterPosFirst + 1, -55.0f},  {"Predator", kHolsterPosFirst + 4, -78.0f},
    {"Predator", kHolsterPosFirst + 7, -33.0f},  {"Alien", kHolsterPosFirst + 1, -55.0f},
    {"Alien", kHolsterPosFirst + 4, -78.0f},     {"Alien", kHolsterPosFirst + 7, -33.0f},
    {"Predator", kLaserSight, 1.0f},             {"Alien", kLaserSight, 1.0f},
    {"Alien", kRoomScaleStep, 1.0f},             {"Alien", kElbowFollowRoll, 0.5f},
    {"Alien", kElbowInKnee, 30.0f},              {"Alien", kElbowInMax, 60.0f},
    {"Alien", kElbowInNearBody, 0.25f},          {"Alien", kElbowOutKnee, 60.0f},
    {"Alien", kElbowOutMax, 110.0f},             {"Alien", kElbowSmoothingMs, 80.0f},
    {"Alien", kLayerAnimation, 1.0f},
};
constexpr ProfileDefault kWeaponDefaults[] = {
    {"m10_2305_197", kShotOriginRight, -0.6f}, // the pistol
    {"m10_2305_197", kMuzzleFlashUp, -2.4f},
    {"m10_2305_197", kMuzzleFlashForward, 16.5f},
    {"m8_2693_284", kMuzzleFlashRight, -0.6f}, // the pulse rifle
    {"m8_2693_284", kMuzzleFlashUp, -1.0f},
    {"m8_2693_284", kMuzzleFlashForward, 44.0f},
    {"m13_3130_690", kMuzzleFlashUp, 3.0f},
    {"m13_3130_690", kMuzzleFlashForward, 50.0f},
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
            for (const ProfileDefault& d : kSpeciesDefaults)
                if (d.index == slot && key == d.key)
                    v = d.value;
        if (&s == &g_weaponSet)
            for (const ProfileDefault& d : kWeaponDefaults)
                if (d.index == slot && key == d.key)
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

// The species set holds every entry the weapon set doesn't, except the Info rows, the Debug group and the player's hand.
void InitSets()
{
    if (!g_speciesSet.slots.empty())
        return;
    for (int i = 0; i < kCount; ++i)
    {
        bool weapon = false;
        for (int w : g_weaponSet.slots)
            weapon = weapon || w == i;
        if (!weapon && i != kLeftHanded && i != kHolsters && i != kHolsterGrabHand && i != kHolsterHoldToKeep &&
            i != kShowGrabZones && kEntries[i].kind != Kind::Info && std::strcmp(kEntries[i].group, "Debug") != 0)
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
    if (std::strcmp(group, "Weapon") == 0 || std::strcmp(group, "Aiming") == 0 || std::strcmp(group, "Flashlight") == 0)
        return marine; // the guns, the flashlight
    if (index > kElbowRollModel && index <= kElbowSmoothingMs)
        return (marine || predator) && g_values[kElbowRollModel].load(std::memory_order_relaxed) != 0.0f;
    if (index == kElbowSwivel) // the natural model's zero is the elbow down: the swivel is the plain IK's
        return (marine || predator) && g_values[kElbowRollModel].load(std::memory_order_relaxed) == 0.0f;
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
    if (!g_padSaved.empty())
        text += "; game controllers hidden from the game in VR (1) or kept (0), by product (VID + PID)\r\n";
    for (const auto& d : g_padSaved)
    {
        snprintf(line, sizeof(line), "HidePadProduct.%s=%d\r\n", d.first.c_str(), d.second ? 1 : 0);
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
        if (key.compare(0, 15, "HidePadProduct.") == 0)
        {
            g_padSaved.emplace_back(key.substr(15), atoi(line.c_str() + eq + 1) != 0);
            continue;
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
                {
                    p.v[j] = g_values[s->slots[j]].load(std::memory_order_relaxed);
                    // A setting newer than the file: the class's own default, if it has one.
                    if (s == &g_speciesSet)
                        for (const ProfileDefault& d : kSpeciesDefaults)
                            if (d.index == s->slots[j] && p.key == d.key)
                                p.v[j] = d.value;
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
bool Rumble() { return g_values[kRumble].load(std::memory_order_relaxed) != 0.0f; }
float RumbleStrength() { return g_values[kRumbleStrength].load(std::memory_order_relaxed); }
float StickFullDeflection() { return g_values[kStickFullDeflection].load(std::memory_order_relaxed); }
float TurnSpeed() { return g_values[kTurnSpeed].load(std::memory_order_relaxed); }

bool GunOnRightController() { return g_values[kGunOnRightController].load(std::memory_order_relaxed) != 0.0f; }
bool ShotsFromGun() { return g_values[kShotsFromGun].load(std::memory_order_relaxed) != 0.0f; }
bool LaserSight() { return g_values[kLaserSight].load(std::memory_order_relaxed) != 0.0f; }
bool MuzzleFlashOnGun() { return g_values[kMuzzleFlashOnGun].load(std::memory_order_relaxed) != 0.0f; }
bool LeftHanded() { return g_values[kLeftHanded].load(std::memory_order_relaxed) != 0.0f; }
void ShoulderOffset(int hand, float off[3])
{
    const int first = hand ? kRightShoulderOutward : kLeftShoulderOutward;
    for (int i = 0; i < 3; ++i)
        off[i] = g_values[first + i].load(std::memory_order_relaxed) * 0.01f; // the settings are centimetres
}
int FlashlightMount() { return static_cast<int>(g_values[kFlashlightMount].load(std::memory_order_relaxed)); }
void FlashlightOffset(float off[3])
{
    for (int i = 0; i < 3; ++i)
        off[i] = g_values[kFlashlightRight + i].load(std::memory_order_relaxed) * 0.01f; // the settings are centimetres
}
bool Holsters() { return g_values[kHolsters].load(std::memory_order_relaxed) != 0.0f; }
bool HolsterGrabHand() { return g_values[kHolsterGrabHand].load(std::memory_order_relaxed) != 0.0f; }
bool HolsterHoldToKeep() { return g_values[kHolsterHoldToKeep].load(std::memory_order_relaxed) != 0.0f; }
bool ShowGrabZones() { return g_values[kShowGrabZones].load(std::memory_order_relaxed) != 0.0f; }
float HolsterRadius() { return g_values[kHolsterRadiusCm].load(std::memory_order_relaxed) * 0.01f; }
float HolsterDrop() { return g_values[kHolsterDropCm].load(std::memory_order_relaxed) * 0.01f; }
void HolsterOffset(int zone, float off[3])
{
    for (int i = 0; i < 3; ++i)
        off[i] = g_values[kHolsterPosFirst + zone * 3 + i].load(std::memory_order_relaxed) * 0.01f;
}
unsigned HolsterActionMask(int zone)
{
    const int i = static_cast<int>(g_values[kHolsterActionFirst + zone].load(std::memory_order_relaxed));
    return kHolsterActions[i >= 0 && i < kHolsterActionCount ? i : 0].mask;
}
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
void MuzzleFlashOffset(float off[3])
{
    for (int i = 0; i < 3; ++i)
        off[i] = g_values[kMuzzleFlashRight + i].load(std::memory_order_relaxed) * 0.01f; // the settings are centimetres
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
ElbowModel ElbowModelSettings()
{
    auto v = [](int i) { return g_values[i].load(std::memory_order_relaxed); };
    ElbowModel m;
    m.enabled = v(kElbowRollModel) != 0.0f;
    m.follow = v(kElbowFollowRoll);
    m.inKneeDeg = v(kElbowInKnee);
    m.inMaxDeg = v(kElbowInMax);
    m.inNearBody = v(kElbowInNearBody);
    m.outKneeDeg = v(kElbowOutKnee);
    m.outMaxDeg = v(kElbowOutMax);
    m.smoothingMs = v(kElbowSmoothingMs);
    return m;
}
int HideBodyBonesFirst() { return static_cast<int>(g_values[kHideBodyBonesFirst].load(std::memory_order_relaxed)); }
int HideBodyBonesLast() { return static_cast<int>(g_values[kHideBodyBonesLast].load(std::memory_order_relaxed)); }
bool DebugMeshProbe() { return g_values[kDebugMeshProbe].load(std::memory_order_relaxed) != 0.0f; }
bool DebugElbowLog() { return g_values[kDebugElbowLog].load(std::memory_order_relaxed) != 0.0f; }
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
    const bool known = FindProfile(s, key) != nullptr;
    s.key = key;
    // A class or a weapon: its own built-in defaults even as the first profile (kSpeciesDefaults, kWeaponDefaults).
    const bool fresh = !s.newFromCurrent;
    if (known || fresh)
    {
        // Its own values, or the defaults the first time.
        const Profile& p = ProfileFor(s, s.key);
        for (size_t j = 0; j < s.slots.size(); ++j)
            g_values[s.slots[j]].store(p.v[j], std::memory_order_relaxed);
    }
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

bool HidePadDevice(const char* id, const char* name)
{
    std::lock_guard<std::mutex> lock(g_fileMutex);
    bool hide = true; // the default: in VR the motion controllers are the game's pad
    bool saved = false;
    for (const auto& d : g_padSaved)
        if (d.first == id)
        {
            hide = d.second;
            saved = true;
        }
    if (!saved)
    {
        g_padSaved.emplace_back(id, hide);
        WriteFileLocked();
    }
    const int n = g_padRowCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (std::strcmp(g_padRows[i].id, id) == 0)
            return g_padRows[i].hide.load(std::memory_order_relaxed);
    if (n < kPadDeviceRows)
    {
        PadDeviceRow& r = g_padRows[n];
        snprintf(r.id, sizeof(r.id), "%s", id);
        snprintf(r.name, sizeof(r.name), "%s", name);
        r.hide.store(hide, std::memory_order_relaxed);
        r.state.store(kPadNotConnected, std::memory_order_relaxed);
        g_padRowCount.store(n + 1, std::memory_order_release);
    }
    return hide;
}

void SetPadDeviceState(const char* id, int state)
{
    const int n = g_padRowCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (!id || std::strcmp(g_padRows[i].id, id) == 0)
            g_padRows[i].state.store(state, std::memory_order_relaxed);
}

bool TakePadDevicesChanged() { return g_padChanged.exchange(false, std::memory_order_relaxed); }

namespace {
std::atomic<const char*> g_motionControllersStatus{"waiting for SteamVR"};
// Two buffers: the dashboard thread reads the one published while the other is written.
char g_motionControllersNames[2][128] = {"Motion controllers", ""};
std::atomic<int> g_motionControllersName{0};
}
void SetMotionControllersStatus(const char* text) { g_motionControllersStatus.store(text, std::memory_order_relaxed); }

void SetMotionControllersName(const char* name)
{
    const int current = g_motionControllersName.load(std::memory_order_acquire);
    if (std::strcmp(g_motionControllersNames[current], name) == 0)
        return;
    const int next = 1 - current;
    snprintf(g_motionControllersNames[next], sizeof(g_motionControllersNames[next]), "%s", name);
    g_motionControllersName.store(next, std::memory_order_release);
    AVP_LOG(kLog, "motion controllers: %s", name);
}

int MenuCount() { return kMenuCount; }

bool GetMenu(int index, MenuInfo* out)
{
    if (index >= kCount && index < kMenuCount && out)
    {
        const int n = g_padRowCount.load(std::memory_order_acquire);
        *out = MenuInfo{};
        out->group = "Game controllers";
        out->unit = "";
        out->text = "";
        if (index == kPadInfoRow)
        {
            out->label = g_motionControllersNames[g_motionControllersName.load(std::memory_order_acquire)];
            out->isInfo = true;
            out->text = g_motionControllersStatus.load(std::memory_order_relaxed);
            return true;
        }
        const int row = (index - kPadInfoRow - 1) / 2;
        const bool toggle = (index - kPadInfoRow - 1) % 2 == 1;
        out->hidden = row >= n;
        if (row >= n)
        {
            out->label = "";
            return true;
        }
        const PadDeviceRow& r = g_padRows[row];
        if (toggle)
        {
            out->label = "    Hide from the game in VR";
            out->isToggle = true;
            out->value = r.hide.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        }
        else
        {
            out->label = r.name;
            out->isInfo = true;
            out->text = PadDeviceStateText(r.state.load(std::memory_order_relaxed));
        }
        return true;
    }
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
    if (index > kPadInfoRow && index < kMenuCount && steps != 0)
    {
        const int row = (index - kPadInfoRow - 1) / 2;
        if ((index - kPadInfoRow - 1) % 2 == 0 || row >= g_padRowCount.load(std::memory_order_acquire))
            return; // a status row
        PadDeviceRow& r = g_padRows[row];
        const bool hide = !r.hide.load(std::memory_order_relaxed);
        r.hide.store(hide, std::memory_order_relaxed);
        AVP_LOG(kLog, "HidePadProduct.%s -> %d", r.id, hide ? 1 : 0);
        std::lock_guard<std::mutex> lock(g_fileMutex);
        for (auto& d : g_padSaved)
            if (d.first == r.id)
                d.second = hide;
        g_padChanged.store(true, std::memory_order_relaxed);
        WriteFileLocked();
        return;
    }
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
