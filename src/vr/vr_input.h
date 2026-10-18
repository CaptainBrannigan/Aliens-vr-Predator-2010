#pragma once
// SteamVR Input (IVRInput_011) for the motion controllers, as a virtual Xbox pad (hooks/xinput_hook.cpp): ported from
// the MOHW mod's openvr_direct/vr_input.cpp. The action manifest and the default Touch bindings are generated here and
// written next to the DLL at connect time, so players rebind in SteamVR's own binding UI (Settings > Controllers >
// Manage controller bindings > AvP), not in a file.
//
// Every pad control is its own SteamVR action ("Gamepad: A" ...); the game resolves them per species and context
// itself, exactly as with a real pad. Default Touch layout: sticks = sticks (and their clicks), triggers = LT / RT
// (analog), A / B / X / Y = A / B / X / Y, thumbrest touch = LB (left) / RB (right), D-pad on double presses
// (X = left, Y = up, B = right, A = down), Start on a held right thumbrest (0.8 s); Back unbound. The hand poses place
// the hands and the gun (hooks/first_person.h); the left grip held on the gun = two-handed aim; the right grip is free.
//
// Log: avpvr_input.log.

namespace vr {
class IVRInput;
class IVRSystem;
}

namespace avp::input {

// Once, after the SteamVR connection is up (main thread).
void Init(::vr::IVRInput* input, ::vr::IVRSystem* system, int trackingUniverse);

// Main thread, once per frame, right after WaitGetPoses.
void Update();

// Main thread: no more IVRInput calls (before VR_Shutdown); the virtual pad goes idle.
void Shutdown();

// The virtual pad, for hooks/xinput_hook.cpp (any thread, lock-free). False until the first update.
struct PadState
{
    unsigned short buttons;           // XINPUT_GAMEPAD_* mask
    unsigned char leftTrigger, rightTrigger; // 0-255
    float moveX, moveY, turnX, turnY; // -1..1, +Y = pushed forward
};
bool GetPad(PadState* out);

// A hand's pose from this frame's update (the same prediction as WaitGetPoses), OpenVR tracking space: 3x4 row-major
// (columns = the controller's x / y / z axes, column 3 = position, metres). palm = the grip pose ("RightHandPalm" /
// "LeftHandPalm" actions), else the aim (tip) pose. hand 0 = left, 1 = right. Any thread (copied under a lock).
bool GetHandPose(int hand, bool palm, float m[3][4]);

// A hand's grip is held (analog grip past 0.7, released below 0.4), from this frame's update. Any thread.
bool GripHeld(int hand);

} // namespace avp::input
