#pragma once
// Screen warp effects off in VR (2026-10-18): the Alien's view has a fisheye -- a full-screen displacement drawn over
// the frame (core/addresses.h vt_ScreenWarp), applied to each eye's image on its own. User-verified to be the fisheye;
// it was NOT the cause of the Alien's doubled meshes (distant things, e.g. through windows, shifting slightly against
// head movement -- still open). Its draw method (slot +0x08) is replaced in the effect
// objects' shadow vtables (hooks/vmt_shadow.h): skipped while VR renders, unless the ScreenWarp setting is on. Each
// effect's name is logged the first time it draws (avpvr_shadow.log).

namespace avp::screen_warp {

// Before StartVmtShadow.
void Register();

} // namespace avp::screen_warp
