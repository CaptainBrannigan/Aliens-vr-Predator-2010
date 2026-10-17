#include "quit_hook.h"

#include "../core/addresses.h"
#include "../core/log.h"
#include "../vr/openvr_session.h"

#include <MinHook.h>
#include <windows.h>

#include <atomic>

namespace avp {
namespace {

constexpr const char* kLog = "avpvr_main.log";

using PFN_PostQuitMessage = void(WINAPI*)(int exitCode);
using PFN_DestroyWindow = BOOL(WINAPI*)(HWND window);
PFN_PostQuitMessage g_realPostQuitMessage = nullptr;
PFN_DestroyWindow g_realDestroyWindow = nullptr;

std::atomic<HWND> g_gameWindow{nullptr};

void WINAPI Hook_PostQuitMessage(int exitCode)
{
    AVP_LOG(kLog, "PostQuitMessage(%d) on thread %lu", exitCode, GetCurrentThreadId());
    vr::ShutdownForExit("PostQuitMessage");
    g_realPostQuitMessage(exitCode);
}

BOOL WINAPI Hook_DestroyWindow(HWND window)
{
    const HWND game = g_gameWindow.load();
    if (window && window == game)
    {
        AVP_LOG(kLog, "DestroyWindow(game window %p) on thread %lu", window, GetCurrentThreadId());
        vr::ShutdownForExit("DestroyWindow (game window)");
    }
    return g_realDestroyWindow(window);
}

template <typename T>
void HookExport(HMODULE module, const char* name, void* detour, T* original)
{
    void* target = reinterpret_cast<void*>(GetProcAddress(module, name));
    MH_STATUS s = target ? MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) : MH_ERROR_FUNCTION_NOT_FOUND;
    if (s == MH_OK)
        s = MH_EnableHook(target);
    AVP_LOG(kLog, "quit hook %s: %s", name, MH_StatusToString(s));
}

} // namespace

void SetGameWindow(void* window)
{
    HWND expected = nullptr;
    if (window && g_gameWindow.compare_exchange_strong(expected, static_cast<HWND>(window)))
        AVP_LOG(kLog, "game window %p", window);
}

void InstallQuitHook()
{
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32)
        user32 = LoadLibraryA("user32.dll");
    if (!user32)
        return;
    HookExport(user32, "PostQuitMessage", reinterpret_cast<void*>(&Hook_PostQuitMessage), &g_realPostQuitMessage);
    HookExport(user32, "DestroyWindow", reinterpret_cast<void*>(&Hook_DestroyWindow), &g_realDestroyWindow);
}

} // namespace avp
