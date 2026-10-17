#pragma once
// EXPERIMENT B3: does the engine render two views side by side if asked to?
//
// When dev::kTwoViewExperiment is on: sets g_ViewCount to 2 at startup, and once per frame (pre-callbacks on
// CamShake_ApplyAllViews and D3D_PresentFrame) copies view 0's camera into view 1, marks view 1 active and sets the
// viewport rects to the left/right halves. Everything it does is logged to avpvr_twoview.log; the snapshot log shows
// the resulting view state. Requires the trace hooks on those two functions (kTraceCamera, kTraceRender).

namespace avp {

void StartTwoViewExperiment();

} // namespace avp
