#pragma once
// Virtual Xbox pad 0 from the motion controllers (vr/vr_input.h), ported from the MOHW mod's hooks/xinput_hook.cpp.
//
// XInputGetState / XInputGetCapabilities are hooked inside xinput1_3.dll (MinHook on the DLL's own code) -- never
// through the exe's import table, which lives in its CEG-checked image. The game's only caller is XInput_PollPad
// (00645AA0). Pad 0 always reports as connected; the controllers' sticks replace a real pad's (when deflected), their
// buttons and triggers are added to it.
//
// Log: avpvr_xinput.log.

namespace avp {

// Once at startup (InitializeMod, after MH_Initialize).
bool InstallXInputHook();

} // namespace avp
