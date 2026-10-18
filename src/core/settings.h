#pragma once
// Player-facing settings: one table (settings.cpp) drives the values, the ini file and the SteamVR dashboard tab
// (vr/dashboard.cpp). Project rule: every adjustable setting is in the dashboard with a meaningful name; nothing is
// ini-only, and obsolete settings are removed rather than left in the file.
//
// File: avpvr_settings.ini next to the DLL, written whenever a value changes (created with the defaults if missing).
// Values are atomics: read from any thread.

namespace avp::settings {

// Reads the file (or writes the defaults). Call once at startup, before anything reads a value.
void Load();

// ---- Values ----
float IpdScale();      // eye separation, x the headset's IPD
bool GameCameraBase(); // eye cameras = the game's camera + the head's difference (keeps shake, recoil)
bool RollFromHeadOnly(); // the game camera's own roll is dropped, the head's roll stays (render only), all species
bool HeadPosition();        // the head's movement since the recenter moves the view (vr/stereo.cpp)
bool EyeFovBetweenFrames(); // view 0 between frames: the game camera's pose with the eyes' field of view (vs. its own)
bool HudPanel();       // HUD squeezed into a panel (off = the game's full-view HUD, corners cut off)
float HudWidthDeg();   // panel width (16:9)
float HudDistance();   // metres: the eyes converge on the panel at this distance
float HudHeightDeg();  // panel centre above straight ahead
bool HideDuplicateTracker(); // marine: hide the flat-screen copy of the motion tracker left outside the panel
bool HideMarineReticle();    // marine: hide the GUI pass's draw 9 (the reticle)
// Head aim's look offset from the view (the playing class's), degrees.
void LookOffsetDeg(float* up, float* right);
bool HeadAim();              // the head drives the player's look / aim (vr/head_aim.h)
float TurnSpeedDeg();        // head aim's smooth turn, right stick fully deflected
float StickFullDeflection(); // VR stick deflection that counts as full (they rarely reach 1.0)
float TurnSpeed();           // right stick scale for the game's own look (head aim off)

bool GunOnRightController(); // Marine: the first-person weapon model on the right controller (hooks/first_person.h)
// Its placement from the controller: off = right, up, forward (metres); angles = pitch (+ up), yaw (+ right), roll
// (degrees, the engine's conventions in the controller's frame).
void GunOffset(float off[3], float angles[3]);
bool ShotsFromGun(); // Marine: shots leave from the gun on the controller, along it (vr/shots.h)
bool LaserSight();   // a beam along the shots' line (vr/laser.h)
// The shots' line on the gun: off = origin right, up, forward from the gun model's origin in its own frame (metres;
// the settings are cm); angles = pitch (+ up), yaw (+ right) from the barrel, degrees.
void ShotOffset(float off[3], float angles[2]);
// Per-weapon values: GunOffset and ShotOffset hold the current weapon's. The weapon changed (hooks/first_person.cpp,
// from the drawn gun's mesh): its profile replaces them -- its own, or the defaults the first time -- after the
// previous weapon's are kept; key = stable id (saved in the ini), name = for the dashboard. Any thread; cheap when
// the weapon is the same.
void SetCurrentWeapon(const char* key, const char* name);
// Per-species values the same way: every setting but the weapon ones and Debug (the Marine's and
// the Predator's bodies differ). key = the species' name (hooks/first_person.cpp, from g_PlayerSpecies).
void SetCurrentSpecies(const char* key, const char* name);
// Two-handed aim (hooks/first_person.cpp, from the MOHW mod): the left grip held with the left palm on the gun's
// barrel line points the gun at it. grabRadius / reach = how far from the line / along it the palm may be (metres);
// grip = the off hand's hold from the barrel line, right / up in the gun's frame (metres).
bool TwoHandedAim();
void TwoHandParams(float* grabRadius, float* reach, float grip[2]);
bool ArmsFollowControllers();  // Marine: each arm + hand follows its controller (hooks/first_person.h)
void BodyOffset(float* forward, float* up); // the first-person body model moved from the view, metres (render only)
float TorsoTurnDeg();   // the body's upper body turned from the stomach about the vertical, + right (render only)
float ElbowSwivelDeg(); // the arm IK's elbows turned about the shoulder-wrist line, + from out toward down
// hand 0 = left, 1 = right; as GunOffset (settings in cm). One setting for both hands, mirrored for the left (its
// right offset, yaw and roll reversed).
void HandOffset(int hand, float off[3], float angles[3]);
int HideBodyBonesFirst();     // bone hide on the first-person body: bones # first..last (1-based), 0 = off
int HideBodyBonesLast();
bool DebugMeshProbe(); // log the first-person body pass's draws (hooks/first_person.h)
int DebugHidePass();   // index into kHudPasses (hooks/hud_passes.h) whose draws are skipped, kHidePassUnlisted, -1 = none
int DebugHideDraw();   // only that pass's Nth draw (per eye render), 0 = all of them

// ---- Dashboard menu ----
struct MenuInfo
{
    const char* group;
    const char* label;
    bool isToggle;
    bool isChoice;   // value cycled with < >; text holds the current choice
    bool isInfo;     // read-only: text only
    bool hidden;     // not used by the class playing (or head aim is off): not shown
    float value;
    int decimals;
    const char* unit;
    const char* text;
};
int MenuCount();
bool GetMenu(int index, MenuInfo* out);
// Toggle (any steps), choice (+-1), number (steps x its step, clamped). Saves the file.
void AdjustMenu(int index, int steps);

} // namespace avp::settings
