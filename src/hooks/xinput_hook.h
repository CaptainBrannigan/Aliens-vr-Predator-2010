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
// Polling isn't enough either (2026-10-31, the same player on a release with only the above: still no input, fine with
// a real pad). The profile's saved controller option defaults to off, and a profile loaded with no pad is forced off;
// while it's off Input_CheckPadDevices wipes the published pad input after every poll, and only a pad lost mid-session
// brings it back by itself. That's the other half of the workaround: "setting controller to on in settings".
// KeepGamePadEnabled turns the option on as well, with the game's own setter. The option gets saved as on.
//
// Nor are those two (2026-11-04, a Pico player: every button reached the hook, the game did nothing; only the mod's own
// stick turn worked). The game binds pad buttons per device DirectInput lists to it, by its type (Xbox / PS3 / generic
// layouts, Input_ClassifyPadName), and takes the last one listed as its pad: with no device listed, no pad button is
// bound anywhere. So IDirectInput8A::EnumDevices is hooked inside dinput8.dll. Once the motion controllers run, the
// game is made to list its devices again (the pad count it compares, set to one it hasn't seen); each device then
// goes through or not by its "Game controllers" setting (all hidden by default: another device's layout would bind
// the same button codes to other actions), and with none left the virtual pad is listed last, as an Xbox pad on
// XInput pad 0. Before the controllers run (flat play) every device is listed as usual.
//
// The controller option is turned on only until the game shows the pad connected with the option on; after that it's
// the player's for the session.
//
// Log: avpvr_xinput.log.

namespace avp {

// Once at startup (InitializeMod, after MH_Initialize).
bool InstallXInputHook();

// Every frame on the game's main thread: while the controllers are running, has the game list its devices again
// (first time, and after a "Game controllers" change), enables its pad on XInput pad 0 if it lists nothing, and turns
// its controller option on until the game reports the pad connected with it on.
void KeepGamePadEnabled();

} // namespace avp
