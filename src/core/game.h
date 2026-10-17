#pragma once
// Which game executable we're loaded into, and whether its build matches the addresses in addresses.h.

namespace avp {

enum class GameBuild
{
    Unknown,   // not an AvP exe, or a build we have no addresses for -- install nothing game-specific
    AvP_DX11,  // AvP_DX11.exe, build stamp 0x4C34AC5D: addresses.h applies
    AvP_DX9,   // AvP.exe (DX9) -- recognized, no address table yet (the dxgi proxy doesn't load there anyway)
};

// Inspects the host exe once (name, PE stamp, image size, a few prologue bytes) and logs the verdict.
GameBuild DetectGameBuild();

const char* GameBuildName(GameBuild build);

} // namespace avp
