#include "game.h"

#include "addresses.h"
#include "log.h"

#include <windows.h>
#include <cstring>

namespace avp {
namespace {

constexpr const char* kLog = "avpvr_main.log";

struct PrologueCheck
{
    uintptr_t address;
    unsigned char bytes[4];
};

// First bytes of a few functions we hook, read from the shipping exe. A mismatch means a patched or different build.
constexpr PrologueCheck kPrologues[] = {
    {addr::Cam_SetRotationMatrix,     {0x8B, 0x44, 0x24, 0x04}},
    {addr::LookCtrl_SetLookDirection, {0x55, 0x8B, 0xEC, 0x83}},
    {addr::D3D_PresentFrame,          {0x83, 0x3D, 0x24, 0x58}},
    {addr::CamComp_ApplyToRenderCamera, {0x81, 0xEC, 0xC0, 0x00}},
};

} // namespace

const char* GameBuildName(GameBuild build)
{
    switch (build)
    {
    case GameBuild::AvP_DX11: return "AvP_DX11.exe (0x4C34AC5D)";
    case GameBuild::AvP_DX9: return "AvP.exe (DX9)";
    default: return "unknown";
    }
}

GameBuild DetectGameBuild()
{
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    const char* exeName = strrchr(exePath, '\\');
    exeName = exeName ? exeName + 1 : exePath;

    auto base = reinterpret_cast<const unsigned char*>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const uint32_t stamp = nt->FileHeader.TimeDateStamp;
    const uint32_t size = nt->OptionalHeader.SizeOfImage;

    AVP_LOG(kLog, "host exe: %s, base 0x%p, stamp 0x%08X, image size 0x%X", exePath, base, stamp, size);

    if (_stricmp(exeName, "AvP.exe") == 0)
    {
        AVP_LOG(kLog, "DX9 executable: recognized but not supported yet -- nothing installed");
        return GameBuild::AvP_DX9;
    }
    if (_stricmp(exeName, "AvP_DX11.exe") != 0)
    {
        AVP_LOG(kLog, "not an AvP executable -- nothing installed");
        return GameBuild::Unknown;
    }
    if (reinterpret_cast<uintptr_t>(base) != addr::kImageBase || stamp != addr::kTimeDateStamp ||
        size != addr::kSizeOfImage)
    {
        AVP_LOG(kLog, "AvP_DX11.exe build mismatch (expected base 0x%X, stamp 0x%08X, size 0x%X) -- nothing installed",
                static_cast<unsigned>(addr::kImageBase), addr::kTimeDateStamp, addr::kSizeOfImage);
        return GameBuild::Unknown;
    }
    for (const PrologueCheck& check : kPrologues)
    {
        if (memcmp(reinterpret_cast<const void*>(check.address), check.bytes, sizeof(check.bytes)) != 0)
        {
            AVP_LOG(kLog, "prologue mismatch at 0x%08X -- exe is patched or different; nothing installed",
                    static_cast<unsigned>(check.address));
            return GameBuild::Unknown;
        }
    }
    AVP_LOG(kLog, "build verified: %s", GameBuildName(GameBuild::AvP_DX11));
    return GameBuild::AvP_DX11;
}

} // namespace avp
