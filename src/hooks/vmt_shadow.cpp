#include "vmt_shadow.h"

#include "trace_hooks.h"
#include "../core/addresses.h"
#include "../core/dev_flags.h"
#include "../core/log.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace avp {
namespace {

constexpr const char* kLog = "avpvr_shadow.log";

// Exe code range: vtable entries pointing here are copied; the first entry that doesn't ends the vtable.
constexpr uintptr_t kTextBegin = 0x00401000;
constexpr uintptr_t kTextEnd = 0x008BC000;
constexpr int kMaxVtableEntries = 128;

using Validator = bool (*)(uintptr_t object);

struct ShadowClass
{
    const char* name;
    uintptr_t vtable;
    Validator validate;
    // Filled in by BuildShadow:
    uintptr_t* shadow = nullptr; // shadow[0] = slot 0
    int entries = 0;
    int replaced = 0;
    std::atomic<int> swapped{0};
};

bool SafeRead32(uintptr_t address, uint32_t* out)
{
    __try
    {
        *out = *reinterpret_cast<const volatile uint32_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool IsUserPointer(uint32_t p) { return p >= 0x10000 && p < 0xFFFF0000u && (p & 3) == 0; }

// A camera component (vtables 0091D694 and its four siblings). CamComp_Ctor (00696D00) stores two fixed .rdata
// addresses in every instance -- +0xA8 = 0091D4B4 and +0xE0 = 0091D4BC (embedded sub-objects) -- and the settings
// pointer (its 3rd argument) at +0x7C. (First attempt, "owner entity at +0x04 points back at +0x28", was wrong: the
// component's methods get the entity as an argument; +0x04 is whatever the constructor's caller passes.)
bool ValidateCameraComponent(uintptr_t object)
{
    uint32_t sub1 = 0, sub2 = 0, settings = 0;
    return SafeRead32(object + 0xA8, &sub1) && sub1 == 0x0091D4B4 && SafeRead32(object + 0xE0, &sub2) &&
           sub2 == 0x0091D4BC && SafeRead32(object + 0x7C, &settings) && IsUserPointer(settings);
}

// A look controller (addresses.h vt_LookCtrl*): ctrl+0x60 points at its own embedded input mapping, ctrl+0x30 or
// ctrl+0x48 (LookCtrl_Ctor sets the first, LookCtrl_Update switches between them); +0x04 is the owner.
bool ValidateLookController(uintptr_t object)
{
    uint32_t mapping = 0, owner = 0;
    return SafeRead32(object + 0x60, &mapping) && (mapping == object + 0x30 || mapping == object + 0x48) &&
           SafeRead32(object + 0x04, &owner) && IsUserPointer(owner);
}

// A ranged weapon (addresses.h vt_RangedWeapons): the base ctor (007B3D40) always sets bit 0x80 in the flags at +0x80,
// and each class's ctor a small type number at +0x40 (the factory 007BBB70's cases, 0..0x15).
bool ValidateRangedWeapon(uintptr_t object)
{
    uint32_t type = 0, flags = 0;
    return SafeRead32(object + 0x40, &type) && type < 0x20 && SafeRead32(object + 0x80, &flags) && (flags & 0x80);
}

// Logs a candidate's identifying fields the first time it's seen (capped), with the validation verdict.
void LogCandidateOnce(const char* className, uintptr_t object, bool valid)
{
    static uintptr_t seen[128];
    static int seenCount = 0;
    for (int i = 0; i < seenCount; ++i)
        if (seen[i] == object)
            return;
    if (seenCount >= 128)
        return;
    seen[seenCount++] = object;
    uint32_t f[6]{};
    const uint32_t offsets[6] = {0x04, 0x08, 0x0C, 0x7C, 0xA8, 0xE0};
    for (int i = 0; i < 6; ++i)
        SafeRead32(object + offsets[i], &f[i]);
    float fov = 0;
    memcpy(&fov, &f[2], 4);
    AVP_LOG(kLog, "candidate %s at %08X: +04=%08X +08=%08X +0C=%08X(%.3f) +7C=%08X +A8=%08X +E0=%08X -> %s", className,
            static_cast<unsigned>(object), f[0], f[1], f[2], fov, f[3], f[4], f[5], valid ? "VALID" : "rejected");
}

ShadowClass g_classes[] = {
    {"CameraComponent 0091D694", 0x0091D694, &ValidateCameraComponent},
    {"CameraComponent 00920054", 0x00920054, &ValidateCameraComponent},
    {"CameraComponent 00921360", 0x00921360, &ValidateCameraComponent},
    {"CameraComponent 009226E4", 0x009226E4, &ValidateCameraComponent},
    {"CameraComponent 00923280", 0x00923280, &ValidateCameraComponent},
    // Look controllers, one class per species (hooks/look_probe.cpp replaces SetLookDirection, +0x9C).
    {"LookController A 00923C88", addr::vt_LookCtrlClassA, &ValidateLookController},
    {"LookController B 009220F8", addr::vt_LookCtrlClassB, &ValidateLookController},
    {"LookController C 00922E40", addr::vt_LookCtrlClassC, &ValidateLookController},
    // Ranged weapons (vr/controller_aim.cpp replaces GetFireTransform, +0xE4).
    {"RangedWeapon 00935708", addr::vt_RangedWeapons[0], &ValidateRangedWeapon},
    {"RangedWeapon 00935820", addr::vt_RangedWeapons[1], &ValidateRangedWeapon},
    {"RangedWeapon 00935930", addr::vt_RangedWeapons[2], &ValidateRangedWeapon},
    {"RangedWeapon 00935A58", addr::vt_RangedWeapons[3], &ValidateRangedWeapon},
    {"RangedWeapon 00935C48", addr::vt_RangedWeapons[4], &ValidateRangedWeapon},
    {"RangedWeapon 00935D60", addr::vt_RangedWeapons[5], &ValidateRangedWeapon},
    {"RangedWeapon 00935E90", addr::vt_RangedWeapons[6], &ValidateRangedWeapon},
    {"RangedWeapon 00935FB0", addr::vt_RangedWeapons[7], &ValidateRangedWeapon},
    {"RangedWeapon 009360C0", addr::vt_RangedWeapons[8], &ValidateRangedWeapon},
    {"RangedWeapon 009361F0", addr::vt_RangedWeapons[9], &ValidateRangedWeapon},
    {"RangedWeapon 00936310", addr::vt_RangedWeapons[10], &ValidateRangedWeapon},
    {"RangedWeapon 00936418", addr::vt_RangedWeapons[11], &ValidateRangedWeapon},
    {"RangedWeapon 00936698", addr::vt_RangedWeapons[12], &ValidateRangedWeapon},
    {"RangedWeapon 009367A8", addr::vt_RangedWeapons[13], &ValidateRangedWeapon},
};
constexpr int kClassCount = static_cast<int>(sizeof(g_classes) / sizeof(g_classes[0]));

struct Replacement
{
    uintptr_t original;
    void* replacement;
};
Replacement g_replacements[16];
int g_replacementCount = 0;

void* ReplacementFor(uintptr_t original)
{
    for (int i = 0; i < g_replacementCount; ++i)
        if (g_replacements[i].original == original)
            return g_replacements[i].replacement;
    return nullptr;
}

bool BuildShadow(ShadowClass& c)
{
    const auto* original = reinterpret_cast<const uintptr_t*>(c.vtable);
    int n = 0;
    while (n < kMaxVtableEntries && original[n] >= kTextBegin && original[n] < kTextEnd)
        ++n;
    if (n == 0)
    {
        AVP_LOG(kLog, "%s: no entries?", c.name);
        return false;
    }
    // One extra slot in front: a copy of vtable[-1] (RTTI locator position in MSVC; unused by this game, copied anyway).
    auto* block = static_cast<uintptr_t*>(
        VirtualAlloc(nullptr, (n + 1) * sizeof(uintptr_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!block)
        return false;
    block[0] = original[-1];
    c.shadow = block + 1;
    c.entries = n;
    for (int i = 0; i < n; ++i)
    {
        c.shadow[i] = original[i];
        if (void* repl = ReplacementFor(original[i]))
        {
            c.shadow[i] = reinterpret_cast<uintptr_t>(repl);
            ++c.replaced;
            AVP_LOG(kLog, "%s: slot %d (+0x%02X) %08X -> replacement %p", c.name, i, i * 4,
                    static_cast<unsigned>(original[i]), repl);
        }
        else if (void* stub = TraceShadowStub(original[i]))
        {
            c.shadow[i] = reinterpret_cast<uintptr_t>(stub);
            ++c.replaced;
            AVP_LOG(kLog, "%s: slot %d (+0x%02X) %08X -> stub %p", c.name, i, i * 4, static_cast<unsigned>(original[i]),
                    stub);
        }
    }
    AVP_LOG(kLog, "%s: shadow vtable at %p, %d entries, %d hooked", c.name, c.shadow, n, c.replaced);
    return c.replaced > 0;
}

struct Candidate
{
    uintptr_t object;
    int classIndex;
};

int ClassIndexOf(uint32_t value)
{
    for (int i = 0; i < kClassCount; ++i)
        if (g_classes[i].shadow && g_classes[i].vtable == value)
            return i;
    return -1;
}

// Scans one region for dwords equal to a registered vtable. SEH-guarded: the region can be freed while we read it.
void ScanRegion(uintptr_t base, size_t size, uint32_t minVt, uint32_t maxVt, std::vector<Candidate>& out)
{
    __try
    {
        const auto* p = reinterpret_cast<const volatile uint32_t*>(base);
        const size_t n = size / 4;
        for (size_t i = 0; i < n; ++i)
        {
            const uint32_t v = p[i];
            if (v < minVt || v > maxVt)
                continue;
            const int ci = ClassIndexOf(v);
            if (ci >= 0 && out.size() < 4096)
                out.push_back({base + i * 4, ci});
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void ScanOnce(unsigned round)
{
    uint32_t minVt = 0xFFFFFFFF, maxVt = 0;
    for (const ShadowClass& c : g_classes)
    {
        if (!c.shadow)
            continue;
        minVt = c.vtable < minVt ? static_cast<uint32_t>(c.vtable) : minVt;
        maxVt = c.vtable > maxVt ? static_cast<uint32_t>(c.vtable) : maxVt;
    }
    if (maxVt == 0)
        return;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t maxAddress = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    const DWORD start = GetTickCount();
    std::vector<Candidate> candidates;
    size_t bytesScanned = 0;
    uintptr_t address = 0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while (address < maxAddress && VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)))
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t next = base + mbi.RegionSize;
        const DWORD writable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & writable) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        {
            ScanRegion(base, mbi.RegionSize, minVt, maxVt, candidates);
            bytesScanned += mbi.RegionSize;
        }
        if (next <= address)
            break;
        address = next;
    }

    int validated = 0, swapped = 0;
    for (const Candidate& cand : candidates)
    {
        ShadowClass& c = g_classes[cand.classIndex];
        const bool valid = c.validate(cand.object);
        LogCandidateOnce(c.name, cand.object, valid);
        if (!valid)
            continue;
        ++validated;
        const LONG shadow = static_cast<LONG>(reinterpret_cast<uintptr_t>(c.shadow));
        const LONG original = static_cast<LONG>(c.vtable);
        if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(cand.object), shadow, original) == original)
        {
            ++swapped;
            c.swapped.fetch_add(1);
            uint32_t owner = 0;
            SafeRead32(cand.object + 4, &owner);
            AVP_LOG(kLog, "swapped %s object %08X (owner entity %08X) -> shadow %p", c.name,
                    static_cast<unsigned>(cand.object), owner, c.shadow);
        }
    }

    const DWORD elapsed = GetTickCount() - start;
    if (swapped || round <= 3 || round % 20 == 0)
        AVP_LOG(kLog, "scan %u: %.0f MB in %lu ms, %zu candidates, %d valid, %d newly swapped", round,
                bytesScanned / (1024.0 * 1024.0), elapsed, candidates.size(), validated, swapped);
}

void ScannerLoop()
{
    for (unsigned round = 1;; ++round)
    {
        ScanOnce(round);
        Sleep(dev::kShadowScanIntervalMs);
    }
}

} // namespace

void RegisterShadowReplacement(uintptr_t originalFunction, void* replacement)
{
    if (g_replacementCount < 16)
        g_replacements[g_replacementCount++] = {originalFunction, replacement};
}

void StartVmtShadow()
{
    int ready = 0;
    for (ShadowClass& c : g_classes)
        if (BuildShadow(c))
            ++ready;
    AVP_LOG(kLog, "%d of %d classes have hooked shadow vtables", ready, kClassCount);
    if (ready == 0)
        return;
    std::thread(ScannerLoop).detach();
}

} // namespace avp
