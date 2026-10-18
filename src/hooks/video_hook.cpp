#include "video_hook.h"

#include "../core/log.h"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstdint>

namespace avp::video {
namespace {

constexpr const char* kLog = "avpvr_main.log";
constexpr uint32_t kMinMovieWidth = 640; // smaller ones: animated textures on in-world screens
constexpr DWORD kPlayingMs = 250;

// HBINK starts with its Width and Height (Bink SDK's BINK struct).
struct BinkHead
{
    uint32_t width, height;
};

using PFN_BinkOpen = void*(__stdcall*)(const char* name, uint32_t flags);
using PFN_BinkDoFrame = int(__stdcall*)(void* bink);
PFN_BinkOpen g_realOpen = nullptr;
PFN_BinkDoFrame g_realDoFrame = nullptr;
std::atomic<DWORD> g_lastMovieFrame{0};

bool IsMovie(void* bink)
{
    __try
    {
        const auto* b = static_cast<const BinkHead*>(bink);
        return b && b->width >= kMinMovieWidth;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadHead(void* bink, uint32_t* w, uint32_t* h)
{
    __try
    {
        *w = static_cast<const BinkHead*>(bink)->width;
        *h = static_cast<const BinkHead*>(bink)->height;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The name argument as text (it can also be a memory source, depending on the flags).
void SafeName(const char* name, char* out, size_t size)
{
    out[0] = 0;
    __try
    {
        size_t i = 0;
        for (; name && i + 1 < size && name[i] >= 0x20 && name[i] < 0x7F; ++i)
            out[i] = name[i];
        out[i] = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out[0] = 0;
    }
}

void* __stdcall Hook_BinkOpen(const char* name, uint32_t flags)
{
    void* bink = g_realOpen(name, flags);
    uint32_t w = 0, h = 0;
    if (bink)
        ReadHead(bink, &w, &h);
    char text[160];
    SafeName(name, text, sizeof(text));
    AVP_LOG(kLog, "Bink video opened: %s (flags %08X), %ux%u%s", text, flags, w, h,
            w >= kMinMovieWidth ? " (a movie: flat screen while it plays)" : "");
    return bink;
}

int __stdcall Hook_BinkDoFrame(void* bink)
{
    if (IsMovie(bink))
        g_lastMovieFrame.store(GetTickCount(), std::memory_order_relaxed);
    return g_realDoFrame(bink);
}

void Hook(HMODULE dll, const char* name, void* detour, void** original)
{
    void* target = reinterpret_cast<void*>(GetProcAddress(dll, name));
    MH_STATUS s = target ? MH_CreateHook(target, detour, original) : MH_ERROR_FUNCTION_NOT_FOUND;
    if (s == MH_OK)
        s = MH_EnableHook(target);
    AVP_LOG(kLog, "video hook %s: %s", name, MH_StatusToString(s));
}

} // namespace

void Install()
{
    HMODULE bink = GetModuleHandleA("binkw32.dll");
    if (!bink)
        bink = LoadLibraryA("binkw32.dll"); // imported by the exe: already loaded, or the game's own copy
    if (!bink)
    {
        AVP_LOG(kLog, "video hooks: binkw32.dll not found");
        return;
    }
    Hook(bink, "_BinkOpen@8", reinterpret_cast<void*>(&Hook_BinkOpen), reinterpret_cast<void**>(&g_realOpen));
    Hook(bink, "_BinkDoFrame@4", reinterpret_cast<void*>(&Hook_BinkDoFrame), reinterpret_cast<void**>(&g_realDoFrame));
}

bool MoviePlaying()
{
    const DWORD last = g_lastMovieFrame.load(std::memory_order_relaxed);
    return last && GetTickCount() - last < kPlayingMs;
}

} // namespace avp::video
