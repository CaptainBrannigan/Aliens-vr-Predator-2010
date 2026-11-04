#include "xinput_hook.h"

#include "../core/addresses.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "first_person.h"
#include "../vr/holsters.h"
#include "../vr/vr_input.h"

#include <MinHook.h>
#include <windows.h>
#include <Xinput.h> // types only: the real functions are reached through the hooks
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h> // types only, as above

#include <atomic>
#include <cmath>
#include <cstdio>
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
    else
    {
        // Head follow (vr/stereo.cpp): the character walks after the player's real head; only while the stick is idle.
        float ax = 0.0f, ay = 0.0f;
        input::GetMoveAssist(&ax, &ay);
        if (ax != 0.0f || ay != 0.0f)
        {
            pad.sThumbLX = ToThumb(ax);
            pad.sThumbLY = ToThumb(ay);
        }
    }
    if (std::fabs(vr.turnX) > kVrStickActiveThreshold || std::fabs(vr.turnY) > kVrStickActiveThreshold)
    {
        const float speed = settings::TurnSpeed();
        pad.sThumbRX = ToThumb(vr.turnX / fullAt * speed);
        pad.sThumbRY = ToThumb(vr.turnY / fullAt * speed);
    }
    // The left grip is the left bumper, and the Marine's two-handed grab on the gun: there it isn't LB.
    pad.wButtons |= holsters::Apply(fp::LeftGripOnGun() ? (vr.buttons & ~XINPUT_GAMEPAD_LEFT_SHOULDER) : vr.buttons);
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
    // Both motors: the controllers rumble (vr_input SetRumble), and the game's per-pad "has vibration" (00A22BE8,
    // FUN_00659D60) follows this.
    caps->Vibration.wLeftMotorSpeed = 0xFFFF;
    caps->Vibration.wRightMotorSpeed = 0xFFFF;
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

// ---- The game's pad device list (DirectInput) ----
// IID_IDirectInput8A (not linking dxguid.lib for one GUID).
const GUID kIID_IDirectInput8A = {0xBF798030, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
// The virtual pad's instance GUID, made up.
const GUID kVirtualPadInstance = {0x41765056, 0x5250, 0x6164, {0x8A, 0x1E, 0x3C, 0x5D, 0x7F, 0x20, 0x26, 0x11}};

using EnumDevicesFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A* self, DWORD devType,
                                                  LPDIENUMDEVICESCALLBACKA callback, LPVOID ref, DWORD flags);
EnumDevicesFn g_originalEnumDevices = nullptr;
bool g_dinputHooked = false;
// Set once the motion controllers run (KeepGamePadEnabled): from then on the game's listings leave out the devices
// hidden in the "Game controllers" settings and end with the virtual pad when nothing else is left. Before that (the
// startup listing, flat play) every device goes through as usual.
std::atomic<bool> g_vrPadMode{false};

struct ListContext
{
    LPDIENUMDEVICESCALLBACKA game;
    LPVOID ref;
    int kept;
    int hidden;
    char lastKept[16]; // product id of the last device passed on: the game's device 0, the one it reads
};

BOOL CALLBACK ListDevice(LPCDIDEVICEINSTANCEA device, LPVOID context)
{
    auto* c = static_cast<ListContext*>(context);
    // One setting per product (its product GUID starts with MAKELONG(VID, PID)), not per device: Steam Input lists up to
    // 15 identical virtual pads, VID 28DE (Valve) PID 11FF, "Controller (XBOX 360 For Windows)" (2026-11-04, the author's
    // PC with no pad connected).
    const DWORD product = device->guidProduct.Data1;
    char id[16], name[MAX_PATH];
    snprintf(id, sizeof(id), "%08lX", product);
    // A short name for the dashboard row: "Controller (XBOX 360 For Windows)" -> "XBOX 360 For Windows".
    const char* productName = device->tszProductName;
    const size_t len = std::strlen(productName);
    if (LOWORD(product) == 0x28DE)
        snprintf(name, sizeof(name), "Steam Input pads");
    else if (len > 13 && std::strncmp(productName, "Controller (", 12) == 0 && productName[len - 1] == ')')
        snprintf(name, sizeof(name), "%.*s", static_cast<int>(len - 13), productName + 12);
    else
        snprintf(name, sizeof(name), "%s", productName);
    const bool vr = g_vrPadMode.load(std::memory_order_relaxed);
    const bool hide = settings::HidePadDevice(id, name) && vr;
    AVP_LOG(kLog, "game controller '%s' (product %s): %s", device->tszProductName, id,
            hide ? "hidden from the game" : vr ? "kept (setting)" : "listed (not in VR yet)");
    if (hide)
    {
        ++c->hidden;
        settings::SetPadDeviceState(id, settings::kPadHidden);
        return DIENUM_CONTINUE;
    }
    ++c->kept;
    settings::SetPadDeviceState(id, settings::kPadNotRead); // until it turns out to be the last one passed on
    snprintf(c->lastKept, sizeof(c->lastKept), "%s", id);
    return c->game(device, c->ref);
}

// Lists the virtual pad to the game last, so it's device 0 -- the game's pad -- with the Xbox button layout.
void AddVirtualPad(LPDIENUMDEVICESCALLBACKA game, LPVOID ref)
{
    DIDEVICEINSTANCEA d{};
    d.dwSize = sizeof(d);
    d.guidInstance = kVirtualPadInstance;
    d.guidProduct.Data1 = 0xFFFFFFFF; // no VID / PID: the game's WMI check matches no device to it
    d.dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
    snprintf(d.tszInstanceName, sizeof(d.tszInstanceName), "AvP VR motion controllers");
    snprintf(d.tszProductName, sizeof(d.tszProductName), "XInput XBOX 360 Pad"); // Input_ClassifyPadName: 5, Xbox
    game(&d, ref);
    // That check doesn't know the device: make the entry just added (the list's head) XInput pad 0, Xbox type.
    const uintptr_t list = Global<uintptr_t>(addr::g_PadDeviceList);
    const uintptr_t node = list ? Global<uintptr_t>(list) : 0;
    const uintptr_t entry = node ? Global<uintptr_t>(node) : 0;
    if (!entry || std::memcmp(reinterpret_cast<const void*>(entry + addr::kPadEntryGuidInstance), &kVirtualPadInstance,
                              sizeof(GUID)) != 0)
    {
        AVP_LOG(kLog, "virtual pad: the game's list has no entry for it (list %08X, head %08X)",
                static_cast<unsigned>(list), static_cast<unsigned>(entry));
        return;
    }
    Global<uint8_t>(entry + addr::kPadEntryIsXInput) = 1;
    Global<int>(entry + addr::kPadEntryXInputIndex) = 0;
    Global<int>(entry + addr::kPadEntryType) = 5;
    AVP_LOG(kLog, "virtual pad listed to the game as its pad (XInput pad 0, Xbox layout), %d device(s)",
            Global<int>(addr::g_PadDeviceCount));
}

HRESULT STDMETHODCALLTYPE HookedEnumDevices(IDirectInput8A* self, DWORD devType, LPDIENUMDEVICESCALLBACKA callback,
                                            LPVOID ref, DWORD flags)
{
    if (callback != reinterpret_cast<LPDIENUMDEVICESCALLBACKA>(addr::Input_EnumPadDevice))
        return g_originalEnumDevices(self, devType, callback, ref, flags);
    ListContext c{callback, ref, 0, 0, {}};
    settings::SetPadDeviceState(nullptr, settings::kPadNotConnected); // the ones listed now get their state back
    const HRESULT hr = g_originalEnumDevices(self, devType, &ListDevice, &c, flags);
    const bool vr = g_vrPadMode.load(std::memory_order_relaxed);
    AVP_LOG(kLog, "the game listed its controllers: %d kept, %d hidden%s", c.kept, c.hidden,
            vr && c.kept == 0 ? "; adding the virtual pad" : "");
    if (vr && c.kept == 0)
        AddVirtualPad(callback, ref);
    else if (c.kept > 0)
        settings::SetPadDeviceState(c.lastKept, settings::kPadSent); // listed last = device 0, the one read
    return hr;
}

// Inside dinput8.dll (MinHook on the DLL's own EnumDevices, found through an IDirectInput8A of our own): the exe's
// vtables and imports stay untouched.
bool InstallDirectInputHook()
{
    HMODULE dinput = LoadLibraryA("dinput8.dll"); // the game's own import
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    const auto create = dinput ? reinterpret_cast<CreateFn>(GetProcAddress(dinput, "DirectInput8Create")) : nullptr;
    IDirectInput8A* di = nullptr;
    const HRESULT hr = create ? create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, kIID_IDirectInput8A,
                                       reinterpret_cast<void**>(&di), nullptr)
                              : E_FAIL;
    if (FAILED(hr) || !di)
    {
        AVP_LOG(kLog, "DirectInput8Create FAILED (0x%08lX) -- no virtual pad in the game's device list", hr);
        return false;
    }
    void* target = (*reinterpret_cast<void***>(di))[4]; // IDirectInput8A::EnumDevices
    di->Release();
    MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void*>(&HookedEnumDevices),
                                reinterpret_cast<void**>(&g_originalEnumDevices));
    if (s == MH_OK)
        s = MH_EnableHook(target);
    AVP_LOG(kLog, "IDirectInput8A::EnumDevices @ %p: %s", target, MH_StatusToString(s));
    g_dinputHooked = s == MH_OK;
    return g_dinputHooked;
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
    InstallDirectInputHook();
    return true;
}

void KeepGamePadEnabled()
{
    static bool lastInUse = false;
    const bool inUse = Global<uint8_t>(addr::g_PadInUse) != 0;
    if (inUse != lastInUse)
    {
        AVP_LOG(kLog, "game's pad in use: %d", inUse);
        lastInUse = inUse;
    }

    input::PadState vr{};
    if (!input::GetPad(&vr)) // no controllers running: leave the game's own pad handling alone
    {
        settings::SetMotionControllersStatus("waiting for SteamVR");
        return;
    }

    // The controllers run: from now on the game's listings follow the "Game controllers" settings. A pad count it
    // hasn't seen makes its next device check (Input_CheckPadDevices) list them again -- its own path for a pad
    // plugged in mid-game, bindings and the player's controller included. Again after a change to those settings.
    const bool devicesChanged = settings::TakePadDevicesChanged();
    if (g_dinputHooked && (!g_vrPadMode.load(std::memory_order_relaxed) || devicesChanged))
    {
        g_vrPadMode.store(true, std::memory_order_relaxed);
        Global<int>(addr::g_PadCapsCount) = -1;
        AVP_LOG(kLog, "the game lists its controllers again (%s)",
                devicesChanged ? "a Game controllers setting changed" : "the motion controllers are running");
    }

    // The game enables its pad itself for an XInput device it lists (the virtual pad included). With nothing listed
    // (no DirectInput, or no hook) it never polls: enable it directly. Not when a kept DirectInput device is the pad.
    const auto pad = reinterpret_cast<void*>(addr::g_Pad);
    if (Global<uint8_t>(addr::g_Pad + addr::kPadXInputEnabled) == 0 &&
        (!g_dinputHooked || Global<int>(addr::g_PadDeviceCount) == 0))
    {
        using PFN_EnablePad = void(__thiscall*)(void* self, int xinputIndex);
        reinterpret_cast<PFN_EnablePad>(addr::XInput_EnablePad)(pad, 0);
        static int logged = 0;
        if (logged < 20)
        {
            AVP_LOG(kLog, "game's pad was off: enabled on XInput pad 0 (%d)", ++logged);
        }
    }

    // The motion controllers' input reaches the game when it reads XInput pad 0 (the virtual pad, or a real / Steam
    // Input pad there, which the hook adds them to) with its controller option on (off: it wipes the pad's input).
    const uintptr_t profile = Global<uintptr_t>(addr::g_PlayerProfile);
    const uintptr_t options = profile ? profile + addr::kProfileControlOptions : 0;
    const bool readsPad0 = Global<uint8_t>(addr::g_Pad + addr::kPadXInputEnabled) != 0 &&
                           Global<int>(addr::g_Pad + addr::kPadXInputIndex) == 0;
    settings::SetMotionControllersStatus(!readsPad0 ? "not sent: another controller is read"
                                         : !options || Global<uint8_t>(options + addr::kControlOptionGamepad) == 0
                                             ? "not sent: the game's controller option is off"
                                             : "input sent to the game");

    // The controller option: turned on until the game itself shows the pad connected with the option on, then the
    // player's to change for the rest of the session (2026-11-04: turned on every frame, a player couldn't switch it
    // off). The next launch checks again.
    static bool optionSettled = false;
    if (optionSettled || !options)
        return;
    if (Global<uint8_t>(options + addr::kControlOptionGamepad) != 0)
    {
        if (Global<uint8_t>(addr::g_PadInUse) != 0)
        {
            optionSettled = true;
            AVP_LOG(kLog, "game's pad connected and its controller option on: left to the player until the next launch");
        }
        return;
    }
    using PFN_SetGamepad = void(__thiscall*)(void* self, bool enabled);
    reinterpret_cast<PFN_SetGamepad>(addr::Options_SetGamepad)(reinterpret_cast<void*>(options), true);
    static int loggedOption = 0;
    if (loggedOption < 20)
    {
        AVP_LOG(kLog, "game's controller option was off: turned on (%d)", ++loggedOption);
    }
}

} // namespace avp
