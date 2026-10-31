#pragma once
// Virtual Xbox pad 0 from the motion controllers (vr/vr_input.h), ported from the MOHW mod's hooks/xinput_hook.cpp.
//
// XInputGetState / XInputGetCapabilities are hooked inside xinput1_3.dll (MinHook on the DLL's own code) -- never
// through the exe's import table, which lives in its CEG-checked image. The game's only caller is XInput_PollPad
// (00645AA0). Pad 0 always reports as connected; the controllers' sticks replace a real pad's (when deflected), their
// buttons and triggers are added to it.
//
// The hooks alone aren't enough: the game only enables its pad object for a device that DirectInput lists as XInput
// (Input_EnumPadDevice, 00645750), and XInput_PollPad doesn't run until then. So a PC with no real or virtual Xbox pad
// never polled at all (2026-10-31, a player's logs: no "pad 0" lines, and the workaround going round was switching an
// Xbox pad on before starting). KeepGamePadEnabled enables it directly.
//
// Log: avpvr_xinput.log.

namespace avp {

// Once at startup (InitializeMod, after MH_Initialize).
bool InstallXInputHook();

// Every frame on the game's main thread: while the controllers are running, keeps the game's pad enabled on XInput
// pad 0. The game's own device check clears it whenever the XInput pad count changes; it then comes straight back.
void KeepGamePadEnabled();

} // namespace avp
