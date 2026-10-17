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
bool HudPanel();       // HUD squeezed into a panel (off = the game's full-view HUD, corners cut off)
float HudWidthDeg();   // panel width (16:9)
float HudDistance();   // metres: the eyes converge on the panel at this distance
float HudHeightDeg();  // panel centre above straight ahead
bool HideDuplicateTracker(); // marine: hide the flat-screen copy of the motion tracker left outside the panel
bool HideMarineReticle();    // marine: hide the GUI pass's draw 9 (the reticle)
// The look's offset from the view for a species (g_PlayerSpecies: 0 Predator, 1 Alien, 2 Marine), degrees.
void AimOffsetDeg(int species, float* up, float* right);
bool HeadAim();              // the head drives the player's look / aim (vr/head_aim.h)
float TurnSpeedDeg();        // head aim's smooth turn, right stick fully deflected
float StickFullDeflection(); // VR stick deflection that counts as full (they rarely reach 1.0)
float TurnSpeed();           // right stick scale for the game's own look (head aim off)

int DebugHidePass();   // index into kHudPasses (hooks/hud_passes.h) whose draws are skipped, kHidePassUnlisted, -1 = none
int DebugHideDraw();   // only that pass's Nth draw (per eye render), 0 = all of them


// ---- Dashboard menu ----
struct MenuInfo
{
    const char* group;
    const char* label;
    bool isToggle;
    bool isChoice;   // value cycled with < >; text holds the current choice
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
