// AvP VR proxy dxgi.dll.
//
// AvP_DX11.exe imports d3d11.dll, which loads dxgi.dll at device creation; a dxgi.dll in the game folder is found
// first. Only the exports d3d11/games actually call are implemented as typed pass-throughs to the real system
// dxgi.dll (loaded by full path) -- MSVC's .def files can't forward by string, see the MOHW mod's proxy notes.
//
// Initialization runs on the first exported call (outside the loader lock: MinHook must not patch under it), with a
// timer fallback in case nothing calls an export. The DX9 exe (AvP.exe) never loads dxgi.dll, so this proxy is
// DX11-only by construction; a DX9 port needs another loader (both exes import dinput8.dll and xinput1_3.dll).

#include "../core/crash_log.h"
#include "../core/dev_flags.h"
#include "../core/game.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../hooks/d3d_probe.h"
#include "../hooks/quit_hook.h"
#include "../hooks/render_twice_experiment.h"
#include "../hooks/screen_warp.h"
#include "../hooks/snapshot.h"
#include "../hooks/trace_hooks.h"
#include "../hooks/two_view_experiment.h"
#include "../hooks/video_hook.h"
#include "../hooks/vmt_shadow.h"
#include "../hooks/xinput_hook.h"
#include "../vr/head_aim.h"
#include "../vr/shots.h"
#include "../vr/stereo.h"

#include <MinHook.h>
#include <windows.h>
#include <unknwn.h>

#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr const char* kLog = "avpvr_main.log";
constexpr unsigned kFallbackInitDelayMs = 15000;

HMODULE RealDxgi()
{
    static HMODULE real = [] {
        char sysDir[MAX_PATH]{};
        GetSystemDirectoryA(sysDir, MAX_PATH); // WOW64 redirects System32 -> SysWOW64 for this 32-bit process
        HMODULE h = LoadLibraryA((std::string(sysDir) + "\\dxgi.dll").c_str());
        AVP_LOG(kLog, h ? "loaded real dxgi.dll" : "FAILED to load real dxgi.dll");
        return h;
    }();
    return real;
}

template <typename Fn>
Fn RealExport(const char* name)
{
    HMODULE h = RealDxgi();
    return h ? reinterpret_cast<Fn>(GetProcAddress(h, name)) : nullptr;
}

void InitializeMod()
{
    AVP_LOG(kLog, "AvP VR proxy initializing (built %s %s), tid %lu", __DATE__, __TIME__, GetCurrentThreadId());

    const avp::GameBuild build = avp::DetectGameBuild();
    if (build != avp::GameBuild::AvP_DX11)
        return; // pass-through only

    const MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
    {
        AVP_LOG(kLog, "MH_Initialize failed: %s", MH_StatusToString(mh));
        return;
    }
    avp::InstallExitHooks();
    avp::settings::Load(); // avpvr_settings.ini (adjusted in the SteamVR dashboard's AvP VR tab)
    avp::InstallXInputHook(); // virtual pad 0 from the motion controllers -- see avpvr_xinput.log
    avp::InstallQuitHook();   // clean SteamVR shutdown when the game quits
    avp::video::Install();    // movies (Bink) to the flat screen

    const int hooks = avp::InstallTraceHooks();
    AVP_LOG(kLog, "%d trace hooks installed -- see avpvr_trace.log", hooks);
    avp::StartTraceSummaryThread();
    avp::StartSnapshotThread();
    avp::stereo::Register(); // camera-apply replacement in the shadow vtables (VR eye cameras)
    avp::aim::Register();    // head aim: look controller SetLookDirection -- see avpvr_aim.log
    avp::shots::Register();  // shots from the gun: the weapons' GetFireTransform -- see avpvr_shots.log
    avp::screen_warp::Register(); // screen warp effects (the Alien's fisheye) skipped in VR
    avp::StartVmtShadow(); // per-object shadow vtables (CEG-safe virtual hooks) -- see avpvr_shadow.log
    avp::StartD3DProbe();  // draw/viewport/present counts from hooks in d3d11/dxgi -- see avpvr_d3d.log
    avp::StartRenderTwiceExperiment();

    if (avp::dev::kTwoViewExperiment)
        avp::StartTwoViewExperiment();

    AVP_LOG(kLog, "initialization done");
}

void EnsureInitialized()
{
    static std::once_flag once;
    std::call_once(once, InitializeMod);
}

} // namespace

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory)
{
    EnsureInitialized();
    auto real = RealExport<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory");
    return real ? real(riid, factory) : E_FAIL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory)
{
    EnsureInitialized();
    auto real = RealExport<HRESULT(WINAPI*)(REFIID, void**)>("CreateDXGIFactory1");
    return real ? real(riid, factory) : E_FAIL;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
{
    EnsureInitialized();
    auto real = RealExport<HRESULT(WINAPI*)(UINT, REFIID, void**)>("CreateDXGIFactory2");
    return real ? real(flags, riid, factory) : E_FAIL;
}

extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debug)
{
    auto real = RealExport<HRESULT(WINAPI*)(UINT, REFIID, void**)>("DXGIGetDebugInterface1");
    return real ? real(flags, riid, debug) : E_NOINTERFACE;
}

extern "C" HRESULT WINAPI DXGIDeclareAdapterRemovalSupport()
{
    auto real = RealExport<HRESULT(WINAPI*)()>("DXGIDeclareAdapterRemovalSupport");
    return real ? real() : S_OK;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        // First thing: crash forensics (avpvr_crash.log). Only CreateFile + AddVectoredExceptionHandler, both fine
        // under the loader lock.
        avp::InstallCrashLogger();
        // Pinned so a FreeLibrary on device teardown can't unload the mod mid-game (seen in MOHW).
        HMODULE pinned = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCSTR>(&DllMain), &pinned);
        // Fallback: if no export is ever called, initialize anyway after a delay. The thread only starts running
        // once the loader lock is released.
        std::thread([] {
            Sleep(kFallbackInitDelayMs);
            EnsureInitialized();
        }).detach();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        if (reserved != nullptr)
            avp::LogProcessExit(); // process exit; nothing else here -- other threads are already gone
        else
            MH_Uninitialize();     // FreeLibrary (shouldn't happen: pinned)
    }
    return TRUE;
}
