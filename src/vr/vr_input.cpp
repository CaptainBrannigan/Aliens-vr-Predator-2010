#include "vr_input.h"

#include "stereo.h"
#include "../core/log.h"

#include <windows.h>
#include <openvr.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace avp::input {
namespace {

constexpr const char* kLog = "avpvr_input.log";
constexpr const char* kManifestFileName = "avpvr_actions.json";
constexpr const char* kTouchBindingsFileName = "avpvr_bindings_oculus_touch.json";

// PAD ACTIONS: the virtual pad's buttons, one SteamVR boolean action each. Names are permanent: SteamVR keeps a saved
// copy of a player's edited bindings keyed by them (MOHW lesson: a renamed action is silently unbound there).
struct PadAction
{
    const char* name; // /actions/main/in/Pad<name>
    const char* label;
    unsigned short mask; // XINPUT_GAMEPAD_* bit
    const char* defaultPath;  // oculus_touch input, nullptr = unbound by default
    const char* defaultMode;  // "button" / "joystick"
    const char* defaultInput; // "click", "double" (double press), "long" (long press), "touch"
    unsigned holdMs = 0;      // > 0: the pad button only once the action has been held this long (timed here)
};
// Start on a held right thumbrest (2026-10-17, replacing MOHW's long Y): the thumbrest is touch-only and SteamVR's
// long press needs a click, so the hold is timed here. The same touch is also RB, held meanwhile.
constexpr unsigned kStartHoldMs = 800;
const PadAction kPadActions[] = {
    {"A", "A", 0x1000, "/user/hand/right/input/a", "button", "click"},
    {"B", "B", 0x2000, "/user/hand/right/input/b", "button", "click"},
    {"X", "X", 0x4000, "/user/hand/left/input/x", "button", "click"},
    {"Y", "Y", 0x8000, "/user/hand/left/input/y", "button", "click"},
    {"LeftStick", "Left stick click", 0x0040, "/user/hand/left/input/joystick", "joystick", "click"},
    {"RightStick", "Right stick click", 0x0080, "/user/hand/right/input/joystick", "joystick", "click"},
    {"DpadUp", "D-pad up", 0x0001, "/user/hand/left/input/y", "button", "double"},
    {"DpadDown", "D-pad down", 0x0002, "/user/hand/right/input/a", "button", "double"},
    {"DpadLeft", "D-pad left", 0x0004, "/user/hand/left/input/x", "button", "double"},
    {"DpadRight", "D-pad right", 0x0008, "/user/hand/right/input/b", "button", "double"},
    {"Start", "Start / pause", 0x0010, nullptr, nullptr, nullptr},
    {"StartHold", "Start / pause (hold 0.8 s)", 0x0010, "/user/hand/right/input/thumbrest", "button", "touch",
     kStartHoldMs},
    {"Back", "Back", 0x0020, nullptr, nullptr, nullptr},
    // The Touch (Quest 3: Touch Plus) thumbrest is capacitive only: touching it holds the bumper.
    {"LB", "Left bumper", 0x0100, "/user/hand/left/input/thumbrest", "button", "touch"},
    {"RB", "Right bumper", 0x0200, "/user/hand/right/input/thumbrest", "button", "touch"},
};
constexpr int kPadActionCount = static_cast<int>(sizeof(kPadActions) / sizeof(kPadActions[0]));

std::string PadActionName(const PadAction& a) { return std::string("/actions/main/in/Pad") + a.name; }

// Labels must never contain a quote followed by a closing parenthesis (they go into raw strings).
std::string BuildActionManifest()
{
    std::string s = R"({
  "default_bindings": [
    { "controller_type": "oculus_touch", "binding_url": "avpvr_bindings_oculus_touch.json" }
  ],
  "actions": [
    { "name": "/actions/main/in/RightHandAim", "type": "pose" },
    { "name": "/actions/main/in/LeftHandAim", "type": "pose" },
    { "name": "/actions/main/in/RightHandPalm", "type": "pose" },
    { "name": "/actions/main/in/LeftHandPalm", "type": "pose" },
    { "name": "/actions/main/in/TriggerRight", "type": "vector1" },
    { "name": "/actions/main/in/TriggerLeft", "type": "vector1" },
    { "name": "/actions/main/in/GripRight", "type": "vector1" },
    { "name": "/actions/main/in/GripLeft", "type": "vector1" },
    { "name": "/actions/main/in/Recenter", "type": "boolean" },
    { "name": "/actions/main/in/Move", "type": "vector2" },
    { "name": "/actions/main/in/Turn", "type": "vector2" })";
    for (const PadAction& a : kPadActions)
        s += ",\n    { \"name\": \"" + PadActionName(a) + "\", \"type\": \"boolean\" }";
    s += R"(
  ],
  "action_sets": [
    { "name": "/actions/main", "usage": "single" }
  ],
  "localization": [
    {
      "language_tag": "en_US",
      "/actions/main": "Gameplay",
      "/actions/main/in/RightHandAim": "Right hand aim - not used yet",
      "/actions/main/in/LeftHandAim": "Left hand aim - not used yet",
      "/actions/main/in/RightHandPalm": "Right hand palm, bind to Grip: places the hand and the gun",
      "/actions/main/in/LeftHandPalm": "Left hand palm, bind to Grip: places the hand, two-handed aim",
      "/actions/main/in/TriggerRight": "Gamepad: right trigger RT",
      "/actions/main/in/TriggerLeft": "Gamepad: left trigger LT",
      "/actions/main/in/GripRight": "Grip - right hand, not used yet",
      "/actions/main/in/GripLeft": "Grip - left hand: hold on the gun for two-handed aim",
      "/actions/main/in/Recenter": "Recenter view",
      "/actions/main/in/Move": "Gamepad: left stick, move",
      "/actions/main/in/Turn": "Gamepad: right stick, look")";
    for (const PadAction& a : kPadActions)
        s += ",\n      \"" + PadActionName(a) + "\": \"Gamepad: " + a.label + "\"";
    s += R"(
    }
  ]
}
)";
    return s;
}

struct DefaultBinding
{
    std::string path, mode, input, output;
};

// A button that also has a double press reports its click only once the double-press window has passed, and then as a
// short pulse (MOHW, live 2026-09-29), so it could never be held. A long press stays true while held, so such a button
// also gets its click's action on "long", after this delay: held past it, the button is held for real until release.
constexpr const char* kHoldLongPressDelay = "0.3";

// The default Touch binding, one source per path + mode (a button can carry several inputs: A = click A + double D-pad
// down); an input bound to two actions (right thumbrest touch = RB and Start-hold) gets a second source.
std::string BuildTouchBindings()
{
    std::vector<DefaultBinding> bindings = {
        {"/user/hand/right/input/trigger", "trigger", "pull", "/actions/main/in/TriggerRight"},
        {"/user/hand/left/input/trigger", "trigger", "pull", "/actions/main/in/TriggerLeft"},
        {"/user/hand/right/input/grip", "trigger", "pull", "/actions/main/in/GripRight"},
        {"/user/hand/left/input/grip", "trigger", "pull", "/actions/main/in/GripLeft"},
        {"/user/hand/left/input/joystick", "joystick", "position", "/actions/main/in/Move"},
        {"/user/hand/right/input/joystick", "joystick", "position", "/actions/main/in/Turn"},
    };
    for (const PadAction& a : kPadActions)
        if (a.defaultPath)
            bindings.push_back({a.defaultPath, a.defaultMode, a.defaultInput, PadActionName(a)});

    std::string sources;
    std::vector<bool> done(bindings.size(), false);
    for (size_t i = 0; i < bindings.size(); ++i)
    {
        if (done[i])
            continue;
        std::string inputs, clickOutput, usedInputs = ",";
        bool hasDouble = false, hasLong = false;
        for (size_t j = i; j < bindings.size(); ++j)
            if (!done[j] && bindings[j].path == bindings[i].path && bindings[j].mode == bindings[i].mode &&
                usedInputs.find("," + bindings[j].input + ",") == std::string::npos)
            {
                done[j] = true;
                usedInputs += bindings[j].input + ",";
                if (!inputs.empty())
                    inputs += ", ";
                inputs += "\"" + bindings[j].input + "\": { \"output\": \"" + bindings[j].output + "\" }";
                if (bindings[j].input == "click")
                    clickOutput = bindings[j].output;
                hasDouble = hasDouble || bindings[j].input == "double";
                hasLong = hasLong || bindings[j].input == "long";
            }
        std::string parameters;
        if (bindings[i].mode == "button" && hasDouble && !hasLong && !clickOutput.empty())
        {
            inputs += ", \"long\": { \"output\": \"" + clickOutput + "\" }"; // see kHoldLongPressDelay
            parameters = std::string(",\n          \"parameters\": { \"long_press_delay\": \"") + kHoldLongPressDelay + "\" }";
        }
        if (!sources.empty())
            sources += ",\n";
        sources += "        {\n          \"path\": \"" + bindings[i].path + "\",\n          \"mode\": \"" + bindings[i].mode +
                   "\",\n          \"inputs\": { " + inputs + " }" + parameters + "\n        }";
    }

    std::string s = R"({
  "controller_type": "oculus_touch",
  "name": "AvP VR defaults",
  "description": "Default AvP VR bindings for Touch controllers: a virtual Xbox pad, thumbrests = bumpers",
  "bindings": {
    "/actions/main": {
      "poses": [
        { "output": "/actions/main/in/RightHandAim", "path": "/user/hand/right/pose/tip" },
        { "output": "/actions/main/in/LeftHandAim", "path": "/user/hand/left/pose/tip" },
        { "output": "/actions/main/in/RightHandPalm", "path": "/user/hand/right/pose/grip" },
        { "output": "/actions/main/in/LeftHandPalm", "path": "/user/hand/left/pose/grip" }
      ],
      "sources": [
)";
    s += sources;
    s += R"(
      ]
    }
  }
}
)";
    return s;
}

bool WriteTextFile(const std::string& path, const std::string& text)
{
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size();
    CloseHandle(h);
    return ok;
}

// Main thread only (Init / Update).
::vr::IVRInput* g_input = nullptr;
::vr::IVRSystem* g_system = nullptr;
::vr::VRActionSetHandle_t g_mainSet = ::vr::k_ulInvalidActionSetHandle;
::vr::VRActionHandle_t g_move = ::vr::k_ulInvalidActionHandle, g_turn = ::vr::k_ulInvalidActionHandle;
::vr::VRActionHandle_t g_trigger[2] = {::vr::k_ulInvalidActionHandle, ::vr::k_ulInvalidActionHandle}; // left, right
::vr::VRActionHandle_t g_recenter = ::vr::k_ulInvalidActionHandle;
::vr::VRActionHandle_t g_grip[2] = {::vr::k_ulInvalidActionHandle, ::vr::k_ulInvalidActionHandle}; // left, right
std::atomic<bool> g_gripHeld[2] = {}; // GripHeld, with hysteresis
::vr::VRActionHandle_t g_palm[2] = {::vr::k_ulInvalidActionHandle, ::vr::k_ulInvalidActionHandle}; // left, right
::vr::VRActionHandle_t g_aim[2] = {::vr::k_ulInvalidActionHandle, ::vr::k_ulInvalidActionHandle};
::vr::ETrackingUniverseOrigin g_universe = ::vr::TrackingUniverseStanding;

// Hand poses for GetHandPose ([hand][palm 1 / aim 0]).
std::mutex g_poseMutex;
float g_poses[2][2][3][4] = {};
bool g_poseValid[2][2] = {};
::vr::VRActionHandle_t g_pad[kPadActionCount] = {};
bool g_ready = false;

// The pad for GetPad (lock-free).
std::atomic<bool> g_havePad{false};
std::atomic<unsigned> g_buttons{0}, g_leftTrigger{0}, g_rightTrigger{0};
std::atomic<float> g_moveX{0.0f}, g_moveY{0.0f}, g_turnX{0.0f}, g_turnY{0.0f};

::vr::VRActionHandle_t Handle(const char* name)
{
    ::vr::VRActionHandle_t h = ::vr::k_ulInvalidActionHandle;
    const ::vr::EVRInputError err = g_input->GetActionHandle(name, &h);
    if (err != ::vr::VRInputError_None)
        AVP_LOG(kLog, "GetActionHandle(%s) err=%d", name, err);
    return h;
}

// Which controllers SteamVR reports (the default binding only applies to "oculus_touch").
void LogControllers()
{
    if (!g_system)
        return;
    for (::vr::TrackedDeviceIndex_t i = 0; i < ::vr::k_unMaxTrackedDeviceCount; ++i)
    {
        if (g_system->GetTrackedDeviceClass(i) != ::vr::TrackedDeviceClass_Controller)
            continue;
        char type[128] = {}, model[128] = {};
        g_system->GetStringTrackedDeviceProperty(i, ::vr::Prop_ControllerType_String, type, sizeof(type));
        g_system->GetStringTrackedDeviceProperty(i, ::vr::Prop_ModelNumber_String, model, sizeof(model));
        AVP_LOG(kLog, "controller %u: type '%s', model '%s'", i, type, model);
    }
}

float Analog1(::vr::VRActionHandle_t action)
{
    ::vr::InputAnalogActionData_t d{};
    if (action == ::vr::k_ulInvalidActionHandle ||
        g_input->GetAnalogActionData(action, &d, sizeof(d), ::vr::k_ulInvalidInputValueHandle) != ::vr::VRInputError_None ||
        !d.bActive)
        return 0.0f;
    return d.x;
}

bool Analog2(::vr::VRActionHandle_t action, float* x, float* y)
{
    ::vr::InputAnalogActionData_t d{};
    *x = *y = 0.0f;
    if (action == ::vr::k_ulInvalidActionHandle ||
        g_input->GetAnalogActionData(action, &d, sizeof(d), ::vr::k_ulInvalidInputValueHandle) != ::vr::VRInputError_None ||
        !d.bActive)
        return false;
    *x = d.x;
    *y = d.y;
    return true;
}

bool Digital(::vr::VRActionHandle_t action, bool* changed = nullptr)
{
    ::vr::InputDigitalActionData_t d{};
    if (changed)
        *changed = false;
    if (action == ::vr::k_ulInvalidActionHandle ||
        g_input->GetDigitalActionData(action, &d, sizeof(d), ::vr::k_ulInvalidInputValueHandle) != ::vr::VRInputError_None ||
        !d.bActive)
        return false;
    if (changed)
        *changed = d.bChanged;
    return d.bState;
}

unsigned TriggerByte(float pull)
{
    if (pull <= 0.0f)
        return 0;
    if (pull >= 1.0f)
        return 255;
    return static_cast<unsigned>(pull * 255.0f + 0.5f);
}

} // namespace

void Init(::vr::IVRInput* input, ::vr::IVRSystem* system, int trackingUniverse)
{
    g_universe = static_cast<::vr::ETrackingUniverseOrigin>(trackingUniverse);
    if (!input || g_input)
        return;
    g_input = input;
    g_system = system;

    const std::string manifestPath = ModuleDirectory() + kManifestFileName;
    const std::string bindingsPath = ModuleDirectory() + kTouchBindingsFileName;
    if (!WriteTextFile(manifestPath, BuildActionManifest()) || !WriteTextFile(bindingsPath, BuildTouchBindings()))
    {
        AVP_LOG(kLog, "FAILED to write the action manifest / bindings next to the DLL (%s)", manifestPath.c_str());
        return;
    }
    // IPCError = our end timed out while vrserver was busy (MOHW, live: it had taken the manifest anyway). Retry, and
    // on a timeout carry on to the handle lookups.
    ::vr::EVRInputError err = ::vr::VRInputError_None;
    for (int attempt = 1; attempt <= 5; ++attempt)
    {
        err = g_input->SetActionManifestPath(manifestPath.c_str());
        AVP_LOG(kLog, "SetActionManifestPath(%s) err=%d (attempt %d)", manifestPath.c_str(), err, attempt);
        if (err != ::vr::VRInputError_IPCError)
            break;
        Sleep(750);
    }
    if (err != ::vr::VRInputError_None && err != ::vr::VRInputError_IPCError)
        return;

    g_input->GetActionSetHandle("/actions/main", &g_mainSet);
    g_move = Handle("/actions/main/in/Move");
    g_turn = Handle("/actions/main/in/Turn");
    g_trigger[0] = Handle("/actions/main/in/TriggerLeft");
    g_trigger[1] = Handle("/actions/main/in/TriggerRight");
    g_palm[0] = Handle("/actions/main/in/LeftHandPalm");
    g_palm[1] = Handle("/actions/main/in/RightHandPalm");
    g_aim[0] = Handle("/actions/main/in/LeftHandAim");
    g_aim[1] = Handle("/actions/main/in/RightHandAim");
    g_recenter = Handle("/actions/main/in/Recenter");
    g_grip[0] = Handle("/actions/main/in/GripLeft");
    g_grip[1] = Handle("/actions/main/in/GripRight");
    for (int i = 0; i < kPadActionCount; ++i)
        g_pad[i] = Handle(PadActionName(kPadActions[i]).c_str());
    g_ready = g_mainSet != ::vr::k_ulInvalidActionSetHandle;
    AVP_LOG(kLog, "SteamVR Input %s (%d pad actions)", g_ready ? "ready" : "NOT ready: no action set", kPadActionCount);
    LogControllers();
}

void Update()
{
    if (!g_ready)
        return;
    ::vr::VRActiveActionSet_t set{};
    set.ulActionSet = g_mainSet;
    const ::vr::EVRInputError err = g_input->UpdateActionState(&set, sizeof(set), 1);
    static ::vr::EVRInputError lastErr = ::vr::VRInputError_None;
    if (err != lastErr)
    {
        AVP_LOG(kLog, "UpdateActionState err=%d", err);
        lastErr = err;
    }
    if (err != ::vr::VRInputError_None)
        return;

    // Hand poses (grip and tip), predicted for the frame WaitGetPoses just started.
    {
        float poses[2][2][3][4] = {};
        bool valid[2][2] = {};
        for (int h = 0; h < 2; ++h)
            for (int k = 0; k < 2; ++k)
            {
                const ::vr::VRActionHandle_t a = k ? g_palm[h] : g_aim[h];
                ::vr::InputPoseActionData_t d{};
                if (a == ::vr::k_ulInvalidActionHandle ||
                    g_input->GetPoseActionDataForNextFrame(a, g_universe, &d, sizeof(d),
                                                           ::vr::k_ulInvalidInputValueHandle) != ::vr::VRInputError_None ||
                    !d.bActive || !d.pose.bPoseIsValid)
                    continue;
                std::memcpy(poses[h][k], d.pose.mDeviceToAbsoluteTracking.m, sizeof(poses[h][k]));
                valid[h][k] = true;
            }
        std::lock_guard<std::mutex> lock(g_poseMutex);
        std::memcpy(g_poses, poses, sizeof(g_poses));
        std::memcpy(g_poseValid, valid, sizeof(g_poseValid));
    }

    float mx, my, tx, ty;
    Analog2(g_move, &mx, &my);
    Analog2(g_turn, &tx, &ty);
    const float lt = Analog1(g_trigger[0]), rt = Analog1(g_trigger[1]);

    // Minimum hold: a click on a button that also has a double press arrives as a pulse as short as one update, and the
    // game samples the pad at its tick rate -- every press is held for at least kPadMinHoldMs (MOHW, live).
    constexpr ULONGLONG kPadMinHoldMs = 100;
    static ULONGLONG heldUntil[kPadActionCount] = {};
    const ULONGLONG now = GetTickCount64();
    unsigned buttons = 0;
    static ULONGLONG downSince[kPadActionCount] = {};
    for (int i = 0; i < kPadActionCount; ++i)
    {
        bool down = Digital(g_pad[i]);
        if (kPadActions[i].holdMs)
        {
            if (!down)
                downSince[i] = 0;
            else if (!downSince[i])
                downSince[i] = now;
            down = down && now - downSince[i] >= kPadActions[i].holdMs;
        }
        if (down && now >= heldUntil[i])
            heldUntil[i] = now + kPadMinHoldMs;
        if (down || now < heldUntil[i])
            buttons |= kPadActions[i].mask;
    }
    bool recenterChanged = false;
    if (Digital(g_recenter, &recenterChanged) && recenterChanged)
        stereo::RequestRecenter();

    // Grips: held from 0.7, released below 0.4 (analog grips rest a little above 0 and wobble around a half press).
    for (int h = 0; h < 2; ++h)
    {
        const float g = Analog1(g_grip[h]);
        const bool was = g_gripHeld[h].load(std::memory_order_relaxed);
        g_gripHeld[h].store(was ? g > 0.4f : g > 0.7f, std::memory_order_relaxed);
    }

    g_moveX.store(mx, std::memory_order_relaxed);
    g_moveY.store(my, std::memory_order_relaxed);
    g_turnX.store(tx, std::memory_order_relaxed);
    g_turnY.store(ty, std::memory_order_relaxed);
    g_buttons.store(buttons, std::memory_order_relaxed);
    g_leftTrigger.store(TriggerByte(lt), std::memory_order_relaxed);
    g_rightTrigger.store(TriggerByte(rt), std::memory_order_relaxed);
    g_havePad.store(true, std::memory_order_release);

    static unsigned lastButtons = 0;
    if (buttons != lastButtons)
    {
        AVP_LOG(kLog, "pad buttons 0x%04X", buttons);
        lastButtons = buttons;
    }
    static ULONGLONG nextLog = 0;
    if (now >= nextLog)
    {
        nextLog = now + 2000;
        AVP_LOG(kLog, "move (%.2f %.2f) turn (%.2f %.2f) LT %.2f RT %.2f buttons 0x%04X", mx, my, tx, ty, lt, rt, buttons);
    }
    static ULONGLONG nextControllerLog = now + 10000; // controllers may connect after the game
    if (now >= nextControllerLog)
    {
        nextControllerLog = ~0ull;
        LogControllers();
    }
}

bool GripHeld(int hand) { return hand >= 0 && hand <= 1 && g_gripHeld[hand].load(std::memory_order_relaxed); }

void Shutdown()
{
    g_ready = false;
    g_havePad.store(false, std::memory_order_release);
    g_gripHeld[0].store(false);
    g_gripHeld[1].store(false);
    AVP_LOG(kLog, "SteamVR Input stopped");
}

bool GetHandPose(int hand, bool palm, float m[3][4])
{
    if (hand < 0 || hand > 1)
        return false;
    std::lock_guard<std::mutex> lock(g_poseMutex);
    if (!g_poseValid[hand][palm ? 1 : 0])
        return false;
    std::memcpy(m, g_poses[hand][palm ? 1 : 0], sizeof(float) * 12);
    return true;
}

bool GetPad(PadState* out)
{
    if (!g_havePad.load(std::memory_order_acquire))
        return false;
    out->buttons = static_cast<unsigned short>(g_buttons.load(std::memory_order_relaxed));
    out->leftTrigger = static_cast<unsigned char>(g_leftTrigger.load(std::memory_order_relaxed));
    out->rightTrigger = static_cast<unsigned char>(g_rightTrigger.load(std::memory_order_relaxed));
    out->moveX = g_moveX.load(std::memory_order_relaxed);
    out->moveY = g_moveY.load(std::memory_order_relaxed);
    out->turnX = g_turnX.load(std::memory_order_relaxed);
    out->turnY = g_turnY.load(std::memory_order_relaxed);
    return true;
}

} // namespace avp::input
