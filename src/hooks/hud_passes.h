#pragma once
// Scene_RenderFrame's calls (00808E00), the "passes" of one 3D frame: the GUI pass (FUN_0079C480 -> FUN_00627EA0, the
// HUD screens) is where the HUD panel squeeze latches; any pass's draws can be hidden from the dashboard's debug rows
// to find which pass draws something (2026-10-17: a second, live motion tracker without the objective marker stays at
// its full-screen place outside the panel). Draw counts per pass go to avpvr_d3d.log.
// In gameplay (2026-10-17 log) the late passes draw nothing except 005FF120 (the main 3D scene) and the GUI pass.
//
// Each entry: the call's return address and the stack slot it sits in while the call runs, in bytes from the slot
// holding Frame_RenderScene's return address (0080A44B, "A"): calls before 00808E93's PUSH EDI use A-4, later
// argument-less calls A-8, the one-argument calls at 00808F0B / 00808F14 (after PUSH ESI) A-16, and the GUI call
// (PUSH 0) A-12, with the 0 argument at A-8. The DAT_009DE288 callbacks (CALL EAX with two pushed arguments) aren't
// listed: the pointer is null in the shipping game as far as seen.

#include <cstdint>

namespace avp {

struct HudPass
{
    uint32_t returnAddress;
    int slot;
    const char* label;
};

constexpr int kGuiPass = 0; // index of the GUI pass

constexpr HudPass kHudPasses[] = {
    {0x00808F38, -12, "GUI pass 0079C480 (HUD)"},
    {0x00808F31, -8, "0063D7C0"},
    {0x00808F2C, -8, "005A5320"},
    {0x00808F27, -8, "004CF5F0"},
    {0x00808F22, -8, "0064AE40 (c)"},
    {0x00808F19, -16, "2D list 00556AB0"},
    {0x00808F10, -16, "2D list 00555F50"},
    {0x00808EF1, -8, "00611DE0"},
    {0x00808EEC, -8, "2D/fades 005FF1D0"},
    {0x00808EE7, -8, "0064AE40 (b)"},
    {0x00808EE2, -8, "0064AE40 (a)"},
    {0x00808ED8, -8, "007F8690"},
    {0x00808ECA, -8, "player draw (vtable)"},
    {0x00808EA8, -8, "0064AE40"},
    {0x00808EA3, -8, "006E5D30"},
    {0x00808E9E, -8, "005FF1B0"},
    {0x00808E99, -8, "005FF120 (3D scene)"},
    {0x00808E81, -4, "005D7C90"},
    {0x00808E7C, -4, "005D8EC0"},
    {0x00808E77, -4, "005D2900"},
    {0x00808E72, -4, "0067BDC0"},
    {0x00808E5B, -4, "005FF0D0"},
    {0x00808E56, -4, "005FEDC0"},
    {0x00808E51, -4, "005FF0B0"},
    {0x00808E4C, -4, "005F9600"},
    {0x00808E47, -4, "0067BDD0"},
    {0x00808E30, -4, "004FBC90"},
    {0x00808E2B, -4, "004F9220"},
    {0x00808E14, -4, "0053AB20"},
    {0x00808E0F, -4, "005D7C50"},
    {0x00808E0A, -4, "0067BDB0"},
    {0x00808E05, -4, "0063D7A0"},
};
constexpr int kHudPassCount = static_cast<int>(sizeof(kHudPasses) / sizeof(kHudPasses[0]));

// The debug hide choice after the listed passes: every draw outside them while VR runs (before / after the scene
// render, between frames, other threads, unlisted calls).
constexpr int kHidePassUnlisted = kHudPassCount;

inline const char* HudPassLabel(int pass)
{
    return pass >= 0 && pass < kHudPassCount ? kHudPasses[pass].label
           : pass == kHidePassUnlisted       ? "unlisted draws (outside the passes)"
                                             : "?";
}

} // namespace avp
