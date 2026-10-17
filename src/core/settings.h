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
bool HudPanel();       // HUD squeezed into a panel (off = the game's full-view HUD, corners cut off)
float HudWidthDeg();   // panel width (16:9)
float HudDistance();   // metres: the eyes converge on the panel at this distance
float HudHeightDeg();  // panel centre above straight ahead
bool HideDuplicateTracker(); // marine: hide the flat-screen copy of the motion tracker left outside the panel
float StickFullDeflection(); // VR stick deflection that counts as full (they rarely reach 1.0)
float TurnSpeed();           // right stick (look) scale
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
