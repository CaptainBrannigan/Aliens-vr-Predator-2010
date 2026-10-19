#include "screen_warp.h"

#include "vmt_shadow.h"
#include "../core/addresses.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "../vr/stereo.h"

#include <windows.h>

#include <cstdint>

namespace avp::screen_warp {
namespace {

constexpr const char* kLog = "avpvr_shadow.log";

using PFN_Draw = void(__thiscall*)(void* effect, float amount);
const auto OriginalDraw = reinterpret_cast<PFN_Draw>(addr::ScreenWarp_Draw);

// The effect's name (its string at +0x08: pointer first), guarded.
void ReadName(const void* effect, char* out, size_t size)
{
    out[0] = 0;
    __try
    {
        const char* s = *reinterpret_cast<const char* const*>(static_cast<const uint8_t*>(effect) + 0x08);
        size_t i = 0;
        for (; s && i + 1 < size && s[i] >= 0x20 && s[i] < 0x7F; ++i)
            out[i] = s[i];
        out[i] = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out[0] = 0;
    }
}

void LogOnce(void* effect, float amount, bool skipped)
{
    static void* seen[32];
    static int seenCount = 0;
    for (int i = 0; i < seenCount; ++i)
        if (seen[i] == effect)
            return;
    if (seenCount >= 32)
        return;
    seen[seenCount++] = effect;
    char name[64];
    ReadName(effect, name, sizeof(name));
    float intensity = 0.0f;
    __try
    {
        intensity = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(effect) + 0x1C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    AVP_LOG(kLog, "screen warp effect %p \"%s\" draws (intensity %.3f, amount %.3f)%s", effect, name, intensity, amount,
            skipped ? ": skipped in VR (ScreenWarp off)" : "");
}

void __fastcall DrawReplacement(void* effect, void* /*edx*/, float amount)
{
    const bool skip = stereo::StereoActive() && !settings::ScreenWarp();
    if (amount != 0.0f)
        LogOnce(effect, amount, skip);
    if (!skip)
        OriginalDraw(effect, amount);
}

} // namespace

void Register()
{
    RegisterShadowReplacement(addr::ScreenWarp_Draw, reinterpret_cast<void*>(&DrawReplacement));
}

} // namespace avp::screen_warp
