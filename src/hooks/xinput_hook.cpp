#include "xinput_hook.h"

#include "../core/log.h"
#include "../core/settings.h"
#include "../vr/vr_input.h"

#include <MinHook.h>
#include <windows.h>
#include <Xinput.h> // types only: the real functions are reached through the hooks

#include <cmath>
#include <cstring>

namespace avp {
namespace {

constexpr const char* kLog = "avpvr_xinput.log";

// Below this a VR stick counts as centred and a real pad's stick is used. Small: the game applies its own deadzone.
constexpr float kVrStickActiveThreshold = 0.05f;

using XInputGetStateFn = DWORD(WINAPI*)(DWORD userIndex, XINPUT_STATE* state);
using XInputGetCapabilitiesFn = DWORD(WINAPI*)(DWORD userIndex, DWORD flags, XINPUT_CAPABILITIES* caps);
XInputGetStateFn g_original = nullptr;
XInputGetCapabilitiesFn g_originalCaps = nullptr;

SHORT ToThumb(float value)
{
    if (value > 1.0f)
        value = 1.0f;
    if (value < -1.0f)
        value = -1.0f;
    return static_cast<SHORT>(std::lround(value * 32767.0f));
}

DWORD WINAPI Hooked(DWORD userIndex, XINPUT_STATE* state)
{
    DWORD result = g_original(userIndex, state);
    if (userIndex != 0 || !state)
        return result;
    // Pad 0 always exists from the game's point of view (it may only look for pads early, before VR is up).
    if (result != ERROR_SUCCESS)
    {
        std::memset(state, 0, sizeof(*state));
        result = ERROR_SUCCESS;
    }
    input::PadState vr{};
    if (!input::GetPad(&vr))
        return result;

    XINPUT_GAMEPAD& pad = state->Gamepad;
    // Outer deadzone: VR sticks often don't reach 1.0 in every direction (MOHW, live: Touch right stick peaked at
    // 0.92). Deflection past StickFullDeflection counts as full; the move stick is scaled by its length to keep its
    // direction.
    float fullAt = settings::StickFullDeflection();
    if (fullAt < 0.5f || fullAt > 1.0f)
        fullAt = 1.0f;
    const float moveLen = std::sqrt(vr.moveX * vr.moveX + vr.moveY * vr.moveY);
    if (moveLen > kVrStickActiveThreshold)
    {
        const float scale = moveLen / fullAt > 1.0f ? 1.0f / moveLen : 1.0f / fullAt;
        pad.sThumbLX = ToThumb(vr.moveX * scale);
        pad.sThumbLY = ToThumb(vr.moveY * scale);
    }
    if (std::fabs(vr.turnX) > kVrStickActiveThreshold || std::fabs(vr.turnY) > kVrStickActiveThreshold)
    {
        const float speed = settings::TurnSpeed();
        pad.sThumbRX = ToThumb(vr.turnX / fullAt * speed);
        pad.sThumbRY = ToThumb(vr.turnY / fullAt * speed);
    }
    pad.wButtons |= vr.buttons;
    if (vr.leftTrigger > pad.bLeftTrigger)
        pad.bLeftTrigger = vr.leftTrigger;
    if (vr.rightTrigger > pad.bRightTrigger)
        pad.bRightTrigger = vr.rightTrigger;

    // Consumers may skip a packet number they've seen: bump it whenever the reported pad changes.
    static XINPUT_GAMEPAD lastReported{};
    static DWORD extraPackets = 0;
    if (std::memcmp(&pad, &lastReported, sizeof(pad)) != 0)
    {
        lastReported = pad;
        ++extraPackets;
    }
    state->dwPacketNumber += extraPackets;

    static ULONGLONG nextLog = 0;
    const ULONGLONG now = GetTickCount64();
    if (now >= nextLog)
    {
        nextLog = now + 2000;
        AVP_LOG(kLog, "pad 0: LX=%d LY=%d RX=%d RY=%d LT=%u RT=%u buttons=0x%04X packet=%lu", pad.sThumbLX, pad.sThumbLY,
                pad.sThumbRX, pad.sThumbRY, pad.bLeftTrigger, pad.bRightTrigger, pad.wButtons, state->dwPacketNumber);
    }
    return result;
}

DWORD WINAPI HookedCaps(DWORD userIndex, DWORD flags, XINPUT_CAPABILITIES* caps)
{
    DWORD result = g_originalCaps(userIndex, flags, caps);
    if (userIndex != 0 || !caps || result == ERROR_SUCCESS)
        return result;
    // No real pad in slot 0: a standard wired gamepad with every control.
    std::memset(caps, 0, sizeof(*caps));
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = 0xFF;
    caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = static_cast<SHORT>(0xFFC0);
    caps->Gamepad.sThumbLY = static_cast<SHORT>(0xFFC0);
    caps->Gamepad.sThumbRX = static_cast<SHORT>(0xFFC0);
    caps->Gamepad.sThumbRY = static_cast<SHORT>(0xFFC0);
    return ERROR_SUCCESS;
}

bool HookExport(HMODULE module, const char* name, void* detour, void** original)
{
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (!target)
    {
        AVP_LOG(kLog, "GetProcAddress(%s) FAILED", name);
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s == MH_OK)
        s = MH_EnableHook(target);
    AVP_LOG(kLog, "%s @ %p: %s", name, target, MH_StatusToString(s));
    return s == MH_OK;
}

} // namespace

bool InstallXInputHook()
{
    // The game's own import; loading it here first is harmless and puts the hook in place before its first poll.
    HMODULE xinput = LoadLibraryA("xinput1_3.dll");
    if (!xinput)
    {
        AVP_LOG(kLog, "LoadLibrary(xinput1_3.dll) FAILED (%lu) -- no virtual pad", GetLastError());
        return false;
    }
    if (!HookExport(xinput, "XInputGetState", reinterpret_cast<void*>(&Hooked), reinterpret_cast<void**>(&g_original)))
        return false;
    HookExport(xinput, "XInputGetCapabilities", reinterpret_cast<void*>(&HookedCaps),
               reinterpret_cast<void**>(&g_originalCaps));
    return true;
}

} // namespace avp
