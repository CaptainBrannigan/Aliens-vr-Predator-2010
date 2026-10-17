#pragma once
// In-process SteamVR (32-bit runtime), the MOHW mod's proven approach: openvr_api.dll is loaded dynamically from the
// mod folder (never linked statically -- the game wouldn't start), VR_InitInternal2(Scene) connects to the running
// vrserver, and IVRSystem / IVRCompositor / IVROverlay are used through the SDK's interface classes.
//
// Nothing here launches SteamVR: connection is only attempted while vrserver.exe is running. All calls are made on
// the game's main thread (camera apply + Present), except the dashboard tab's overlay calls (its own thread; all
// IVROverlay calls take OverlayMutex()).
//
// Also owns the flat "screen" overlay used for everything that isn't head-tracked gameplay (menus, FMV movies,
// loading screens, cutscenes): a world-locked quad placed in front of the head when it appears.

#include <cstdint>
#include <mutex>

struct ID3D11Texture2D;

namespace avp::vr {

struct EyeGeometry
{
    // OpenVR GetProjectionRaw tangents: left/right (x), top/bottom (y pointing DOWN, so top < 0 < bottom).
    float tanLeft, tanRight, tanTop, tanBottom;
    // Eye-to-head translation (meters), x is the half-IPD (left negative).
    float offset[3];
};

struct Pose
{
    float m[3][4]; // HMD to tracking space (OpenVR: right-handed, +Y up, -Z forward)
    bool valid;
};

// Attempts a connection if not connected (throttled to every 2 s, and only while vrserver.exe runs).
void TryConnect();
bool IsConnected();

const EyeGeometry& Eye(int eye);

// Blocks until the compositor wants the next frame and returns the predicted HMD pose. One call per submitted frame.
Pose WaitPoses();

// Submits both eyes. Bounds are u/v sub-rectangles (0..1) of each texture.
void SubmitEyes(ID3D11Texture2D* left, const float boundsLeft[4], ID3D11Texture2D* right, const float boundsRight[4]);

// Flat screen: show `texture` on the virtual screen (placed in front of `head` when it was hidden), or hide it.
void ShowScreen(ID3D11Texture2D* texture, float texelAspect, const Pose& head);
void HideScreen();

// Serializes IVROverlay calls between the main thread (screen) and the dashboard thread (vr/dashboard.cpp).
std::mutex& OverlayMutex();

} // namespace avp::vr
