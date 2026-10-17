#include "trace_hooks.h"

#include "../core/addresses.h"
#include "../core/dev_flags.h"
#include "../core/log.h"

#include <MinHook.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>

namespace avp {
namespace {

constexpr const char* kLog = "avpvr_trace.log";

// ---- What to dump on a detailed line ----

enum class DumpKind : uint8_t
{
    None,
    ArgInts,      // `count` stack args from `arg`, as hex
    ArgFloats,    // `count` stack args from `arg`, as floats
    PtrArgFloats, // `count` floats behind the pointer in stack arg `arg`
    EcxInts,      // `count` dwords at ecx+off, as hex
    EcxFloats,    // `count` floats at ecx+off
};

struct Dump
{
    DumpKind kind = DumpKind::None;
    uint8_t arg = 0;
    uint16_t off = 0;
    uint8_t count = 0;
};

constexpr Dump None() { return {}; }
constexpr Dump ArgInts(uint8_t arg, uint8_t n) { return {DumpKind::ArgInts, arg, 0, n}; }
constexpr Dump ArgFloats(uint8_t arg, uint8_t n) { return {DumpKind::ArgFloats, arg, 0, n}; }
constexpr Dump PtrFloats(uint8_t arg, uint8_t n) { return {DumpKind::PtrArgFloats, arg, 0, n}; }
constexpr Dump EcxInts(uint16_t off, uint8_t n) { return {DumpKind::EcxInts, 0, off, n}; }
constexpr Dump EcxFloats(uint16_t off, uint8_t n) { return {DumpKind::EcxFloats, 0, off, n}; }

constexpr int kMaxSlots = 8;

struct TraceDef
{
    const char* name;
    uintptr_t address;
    uint32_t group;
    Dump dump;
    bool hot; // detailed lines only for the first calls, never throttled-repeating
    // Slot hooks (vtable entries, IAT entries): pointer slots that hold `address`. When any are given, the hook is
    // installed by writing the stub's address into each slot instead of patching code -- the only kind allowed on
    // CEG-checked game code (see dev_flags.h). Only calls made through these slots are seen.
    uintptr_t slots[kMaxSlots];
    // Shadow entry: the stub isn't installed anywhere by this file; vmt_shadow.cpp puts it into per-object shadow
    // vtables (heap memory) wherever the original vtable holds `address`. CEG-safe.
    bool shadow;
};

constexpr bool IsSlotHook(const TraceDef& def) { return def.slots[0] != 0; }

using namespace dev;

// Argument numbering: arg 0 = first *stack* argument. For __thiscall/__fastcall `this` is ECX, not an argument.
// Signatures come from the Ghidra decompiles (docs/re/FINDINGS.md).
const TraceDef kTraces[] = {
    // Camera
    {"Cam_SetPosition",             addr::Cam_SetPosition,             kTraceCamera, PtrFloats(0, 3), false},
    {"Cam_SetRotationMatrix",       addr::Cam_SetRotationMatrix,       kTraceCamera, PtrFloats(0, 9), false},
    {"Cam_SetRotationQuat",         addr::Cam_SetRotationQuat,         kTraceCamera, PtrFloats(0, 4), false},
    {"Cam_SetHorizontalFov",        addr::Cam_SetHorizontalFov,        kTraceCamera, ArgFloats(0, 2), false},
    {"Cam_SetVerticalFov",          addr::Cam_SetVerticalFov,          kTraceCamera, ArgFloats(0, 2), false},
    {"Cam_RecomputeFrustum",        addr::Cam_RecomputeFrustum,        kTraceCamera, None(), false},
    {"Cam_InitDefaults",            addr::Cam_InitDefaults,            kTraceCamera, None(), false},
    {"Cam_SetClipE0",               addr::Cam_SetClipE0,               kTraceCamera, ArgFloats(0, 1), false},
    {"Cam_SetClipDC",               addr::Cam_SetClipDC,               kTraceCamera, ArgFloats(0, 1), false},
    {"Cam_SetViewportRect",         addr::Cam_SetViewportRect,         kTraceCamera, PtrFloats(0, 4), false},
    {"Cam_FlyCamUpdate",            addr::Cam_FlyCamUpdate,            kTraceCamera, None(), false},
    {"CamComp_ApplyToRenderCamera", addr::CamComp_ApplyToRenderCamera, kTraceCamera, EcxFloats(0x118, 12), false},
    {"CamComp_UpdatePosition",      addr::CamComp_UpdatePosition,      kTraceCamera, EcxFloats(0x58, 3), false},
    {"CamComp_SetMode",             addr::CamComp_SetMode,             kTraceCamera, ArgInts(0, 1), false},
    {"Entity_TickCameraComponent",  addr::Entity_TickCameraComponent,  kTraceCamera, EcxInts(0x28, 1), false},
    {"Cinematic_ApplyCamera",       addr::Cinematic_ApplyCamera,       kTraceCamera, EcxFloats(0x38, 12), false},
    {"Cinematic_SetClipPlanes",     addr::Cinematic_SetClipPlanes,     kTraceCamera, ArgFloats(0, 2), false},
    {"CamShake_ApplyAllViews",      addr::CamShake_ApplyAllViews,      kTraceCamera, None(), false},
    {"Cam_CopyFromObject",          addr::Cam_CopyFromObject,          kTraceCamera, EcxFloats(0x4, 12), false},

    // Render / D3D
    {"Render_SetupViewportAspect",  addr::Render_SetupViewportAspect,  kTraceRender, ArgInts(0, 4), false},
    {"Render_BeginFrame",           addr::Render_BeginFrame,           kTraceRender, None(), false},
    {"Render_EndFrame",             addr::Render_EndFrame,             kTraceRender, None(), false},
    {"Render_FinishAndPresent",     addr::Render_FinishAndPresent,     kTraceRender, None(), false},
    {"D3D_PresentFrame",            addr::D3D_PresentFrame,            kTraceRender, None(), false},
    {"D3D_CreateDeviceAndSwapChain",addr::D3D_CreateDeviceAndSwapChain,kTraceRender, None(), false},
    {"D3D_HandleModeChange",        addr::D3D_HandleModeChange,        kTraceRender, ArgInts(0, 2), false},

    // Multi-view
    {"View_ForEachUpdateObjects",   addr::View_ForEachUpdateObjects,   kTraceViews, None(), false},
    {"ShadowOrReflect_RenderPerView", addr::ShadowOrReflect_RenderPerView, kTraceViews, None(), false},
    {"Decal_SubmitForViews",        addr::Decal_SubmitForViews,        kTraceViews, ArgInts(1, 1), false},
    {"View_ComputeVisibilityMask",  addr::View_ComputeVisibilityMask,  kTraceViewsHot, None(), true},
    {"View_ComputeBoundsVisibility",addr::View_ComputeBoundsVisibility,kTraceViewsHot, None(), true},
    {"View_SelectAndSortKey",       addr::View_SelectAndSortKey,       kTraceViewsHot, ArgInts(0, 1), true},
    {"RenderQueue_SubmitMeshPerView", addr::RenderQueue_SubmitMeshPerView, kTraceViewsHot, ArgInts(3, 1), true},
    {"RenderQueue_SubmitPerView",   addr::RenderQueue_SubmitPerView,   kTraceViewsHot, ArgInts(1, 1), true},
    {"Mesh_SubmitForViews",         addr::Mesh_SubmitForViews,         kTraceViewsHot, ArgInts(3, 1), true},
    {"Gui_MapRectToViewViewport",   addr::Gui_MapRectToViewViewport,   kTraceViewsHot, ArgInts(4, 1), true},

    // Look
    {"LookCtrl_Update",             addr::LookCtrl_Update,             kTraceLook, None(), false},
    {"LookCtrlSub_Update",          addr::LookCtrlSub_Update,          kTraceLook, None(), false},
    {"LookCtrl_UpdateLook",         addr::LookCtrl_UpdateLook,         kTraceLook, ArgFloats(1, 7), false},
    {"LookCtrl_SetLookDirection",   addr::LookCtrl_SetLookDirection,   kTraceLook, PtrFloats(1, 4), false},
    {"LookCtrlSub_HandleActions678",addr::LookCtrlSub_HandleActions678,kTraceLook, None(), false},
    {"LookCtrl_GetAction",          addr::LookCtrl_GetAction,          kTraceLookHot, ArgInts(0, 2), true},

    // Input
    {"Input_PackActionBits",        addr::Input_PackActionBits,        kTraceInput, None(), false},
    {"Input_ConsumeAction",         addr::Input_ConsumeAction,         kTraceInput, ArgInts(0, 1), false},
    {"XInput_PollPad",              addr::XInput_PollPad,              kTraceInput, EcxFloats(0x248, 4), false},
    {"XInput_SetRumble",            addr::XInput_SetRumble,            kTraceInput, ArgFloats(1, 2), false},
    {"Input_GetActionValue",        addr::Input_GetActionValue,        kTraceInputHot, ArgInts(0, 1), true},

    // Character
    {"Char_SelectSkinAttachment",   addr::Char_SelectSkinAttachment,   kTraceCharacter, EcxInts(0xE8, 1), false},
    {"Entity_CreateVisionAndWeaponComps", addr::Entity_CreateVisionAndWeaponComps, kTraceCharacter, None(), false},

    // Console
    {"Console_Tick",                addr::Console_Tick,                kTraceConsole, None(), false},
    {"Console_Show",                addr::Console_Show,                kTraceConsole, ArgInts(0, 2), false},
    {"Console_Close",               addr::Console_Close,               kTraceConsole, None(), false},
    {"Console_IsAllowed",           addr::Console_IsAllowed,           kTraceConsole, None(), false},
    {"Console_Draw",                addr::Console_Draw,                kTraceConsole, EcxFloats(0x8, 2), false},

    // ---- Shadow entries (CEG-safe): stubs for per-object shadow vtables, see hooks/vmt_shadow.h ----
    {"SH CamComp_ApplyToRenderCamera", addr::CamComp_ApplyToRenderCamera, kTraceCameraShadow, EcxFloats(0x118, 12),
     false, {}, true},
    {"SH CamComp_UpdatePosition", addr::CamComp_UpdatePosition, kTraceCameraShadow, EcxFloats(0x58, 3), false, {},
     true},

    // ---- Slot hooks (vtable / IAT): write into the exe image -- CEG-UNSAFE for exe slots ----
    // Camera component vtable +0x08 in all five camera classes (0091D694, 00920054, 00921360, 009226E4, 00923280).
    {"VT CamComp_ApplyToRenderCamera", addr::CamComp_ApplyToRenderCamera, kTraceCameraVT, EcxFloats(0x118, 12), false,
     {addr::vt_CamComp_Apply_Slots[0], addr::vt_CamComp_Apply_Slots[1], addr::vt_CamComp_Apply_Slots[2],
      addr::vt_CamComp_Apply_Slots[3], addr::vt_CamComp_Apply_Slots[4]}},
};

constexpr int kTraceCount = static_cast<int>(sizeof(kTraces) / sizeof(kTraces[0]));
constexpr int kCallerSlots = 8;
constexpr uint32_t kFirstDetailedCalls = 3;
constexpr size_t kStubSize = 64;

struct CallerSlot
{
    std::atomic<uint32_t> address{0};
    std::atomic<uint32_t> count{0};
};

struct TraceState
{
    std::atomic<uint32_t> total{0};
    std::atomic<uint32_t> window{0};
    std::atomic<uint32_t> detailed{0};
    std::atomic<uint32_t> lastDetailTick{0};
    std::atomic<uint32_t> firstThread{0};
    std::atomic<bool> multiThreadLogged{false};
    std::atomic<uint32_t> otherCallers{0};
    CallerSlot callers[kCallerSlots];
    std::atomic<TracePreFn> pre{nullptr};
    bool installed = false;
};

TraceState g_state[kTraceCount];
void* g_trampolines[kTraceCount]; // read by the stubs' final `jmp [slot]`
void* g_stubs[kTraceCount];       // each trace's detour stub
thread_local bool t_inHook = false;

bool SafeCopy(void* dst, const void* src, size_t bytes)
{
    __try
    {
        memcpy(dst, src, bytes);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Appends the typed dump for `def` to `out` (bounded).
void FormatDump(const TraceDef& def, const TraceCtx* ctx, char* out, size_t outSize)
{
    const Dump& d = def.dump;
    if (d.kind == DumpKind::None || d.count == 0)
        return;

    uint32_t raw[16]{};
    const int n = d.count > 16 ? 16 : d.count;
    bool ok = true;
    switch (d.kind)
    {
    case DumpKind::ArgInts:
    case DumpKind::ArgFloats:
        ok = SafeCopy(raw, &ctx->args[d.arg], n * 4);
        break;
    case DumpKind::PtrArgFloats:
    {
        uint32_t ptr = 0;
        ok = SafeCopy(&ptr, &ctx->args[d.arg], 4) && ptr && SafeCopy(raw, reinterpret_cast<void*>(ptr), n * 4);
        break;
    }
    case DumpKind::EcxInts:
    case DumpKind::EcxFloats:
        ok = ctx->ecx && SafeCopy(raw, reinterpret_cast<void*>(ctx->ecx + d.off), n * 4);
        break;
    default:
        break;
    }

    size_t used = strlen(out);
    if (!ok)
    {
        snprintf(out + used, outSize - used, " dump=<unreadable>");
        return;
    }
    const bool asFloat = d.kind == DumpKind::ArgFloats || d.kind == DumpKind::PtrArgFloats || d.kind == DumpKind::EcxFloats;
    used += snprintf(out + used, outSize - used, " dump=[");
    for (int i = 0; i < n && used < outSize; ++i)
    {
        if (asFloat)
        {
            float f;
            memcpy(&f, &raw[i], 4);
            used += snprintf(out + used, outSize - used, i ? " %.4f" : "%.4f", f);
        }
        else
        {
            used += snprintf(out + used, outSize - used, i ? " %08X" : "%08X", raw[i]);
        }
    }
    if (used < outSize)
        snprintf(out + used, outSize - used, "]");
}

void RecordCaller(int index, const TraceCtx* ctx)
{
    TraceState& s = g_state[index];
    const uint32_t ret = ctx->ret;
    for (CallerSlot& slot : s.callers)
    {
        uint32_t current = slot.address.load(std::memory_order_relaxed);
        if (current == ret)
        {
            slot.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (current == 0)
        {
            uint32_t expected = 0;
            if (slot.address.compare_exchange_strong(expected, ret))
            {
                slot.count.fetch_add(1, std::memory_order_relaxed);
                AVP_LOG(kLog, "NEW CALLER %s <- ret %08X (ecx %08X, tid %u)", kTraces[index].name, ret, ctx->ecx,
                        GetCurrentThreadId());
                return;
            }
            if (expected == ret)
            {
                slot.count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }
    s.otherCallers.fetch_add(1, std::memory_order_relaxed);
}

void __cdecl OnTraceHit(uint32_t index, const TraceCtx* ctx)
{
    if (t_inHook || index >= static_cast<uint32_t>(kTraceCount))
        return;
    t_inHook = true;
    const DWORD lastError = GetLastError();

    const TraceDef& def = kTraces[index];
    TraceState& s = g_state[index];
    const uint32_t callNo = s.total.fetch_add(1, std::memory_order_relaxed) + 1;
    s.window.fetch_add(1, std::memory_order_relaxed);

    const uint32_t tid = GetCurrentThreadId();
    uint32_t first = 0;
    if (!s.firstThread.compare_exchange_strong(first, tid) && first != tid &&
        !s.multiThreadLogged.exchange(true))
        AVP_LOG(kLog, "THREADS %s called from tid %u as well as tid %u", def.name, tid, first);

    RecordCaller(static_cast<int>(index), ctx);

    // Detailed line: the first few calls, then (non-hot hooks only) at most one per kTraceDetailIntervalMs.
    bool detail = false;
    if (s.detailed.load(std::memory_order_relaxed) < kFirstDetailedCalls)
    {
        detail = s.detailed.fetch_add(1) < kFirstDetailedCalls;
    }
    else if (!def.hot)
    {
        const uint32_t now = GetTickCount();
        uint32_t last = s.lastDetailTick.load(std::memory_order_relaxed);
        if (now - last >= kTraceDetailIntervalMs && s.lastDetailTick.compare_exchange_strong(last, now))
            detail = true;
    }
    if (detail)
    {
        uint32_t a[4]{};
        SafeCopy(a, &ctx->args[0], sizeof(a));
        char line[640];
        snprintf(line, sizeof(line), "%s #%u tid=%u ret=%08X ecx=%08X edx=%08X args=[%08X %08X %08X %08X]",
                 def.name, callNo, tid, ctx->ret, ctx->ecx, ctx->edx, a[0], a[1], a[2], a[3]);
        FormatDump(def, ctx, line, sizeof(line));
        AVP_LOG(kLog, "%s", line);
    }

    if (TracePreFn pre = s.pre.load(std::memory_order_acquire))
        pre(ctx);

    SetLastError(lastError);
    t_inHook = false;
}

// Builds the detour stub for trace `index` at `p`. See trace_hooks.h for what it does.
void BuildStub(uint8_t* p, uint32_t index)
{
    auto emit = [&p](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes)
            *p++ = b;
    };
    auto emit32 = [&p](uint32_t v) {
        memcpy(p, &v, 4);
        p += 4;
    };
    emit({0x9C});                         // pushfd
    emit({0x60});                         // pushad
    emit({0x8B, 0xEC});                   // mov ebp, esp          (ebp = TraceCtx*)
    emit({0x81, 0xEC}); emit32(0x210);    // sub esp, 0x210
    emit({0x83, 0xE4, 0xF0});             // and esp, -16
    emit({0x0F, 0xAE, 0x04, 0x24});       // fxsave [esp]          (x87 + SSE state)
    emit({0xFC});                         // cld
    emit({0x55});                         // push ebp              (ctx)
    emit({0x68}); emit32(index);          // push index
    emit({0xB8}); emit32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&OnTraceHit))); // mov eax, OnTraceHit
    emit({0xFF, 0xD0});                   // call eax
    emit({0x83, 0xC4, 0x08});             // add esp, 8
    emit({0x0F, 0xAE, 0x0C, 0x24});       // fxrstor [esp]
    emit({0x8B, 0xE5});                   // mov esp, ebp
    emit({0x61});                         // popad
    emit({0x9D});                         // popfd
    emit({0xFF, 0x25}); emit32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_trampolines[index]))); // jmp [slot]
}

void SummaryLoop()
{
    unsigned round = 0;
    for (;;)
    {
        Sleep(kTraceSummaryIntervalMs);
        ++round;
        const float seconds = kTraceSummaryIntervalMs / 1000.0f;
        AVP_LOG(kLog, "---- summary %u (last %.0f s) ----", round, seconds);
        for (int i = 0; i < kTraceCount; ++i)
        {
            TraceState& s = g_state[i];
            if (!s.installed)
                continue;
            const uint32_t window = s.window.exchange(0);
            if (window == 0)
                continue;
            AVP_LOG(kLog, "  %-34s %8u calls  %9.1f/s  total %u", kTraces[i].name, window, window / seconds,
                    s.total.load());
        }
        // Caller tables every 6th summary (30 s by default): which return addresses call each hook, and how often.
        if (round % 6 == 0)
        {
            AVP_LOG(kLog, "---- callers (cumulative) ----");
            for (int i = 0; i < kTraceCount; ++i)
            {
                TraceState& s = g_state[i];
                if (!s.installed || s.total.load() == 0)
                    continue;
                char line[512];
                int used = snprintf(line, sizeof(line), "  %s:", kTraces[i].name);
                for (CallerSlot& slot : s.callers)
                {
                    const uint32_t address = slot.address.load();
                    if (address && used < static_cast<int>(sizeof(line)))
                        used += snprintf(line + used, sizeof(line) - used, " %08X x%u", address, slot.count.load());
                }
                if (s.otherCallers.load() && used < static_cast<int>(sizeof(line)))
                    snprintf(line + used, sizeof(line) - used, " (+%u from more callers)", s.otherCallers.load());
                AVP_LOG(kLog, "%s", line);
            }
        }
    }
}

// Points every slot of trace `index` at its stub. The stub's final `jmp [g_trampolines[index]]` goes to the original
// function pointer, so nothing in the game's code changes. Every slot must still hold the expected function
// (a mismatch means another hook got there first, or a different build) -- those slots are skipped.
bool InstallSlotHook(int index, uint8_t* stub)
{
    const TraceDef& def = kTraces[index];
    g_trampolines[index] = reinterpret_cast<void*>(def.address);
    int patched = 0;
    for (uintptr_t slot : def.slots)
    {
        if (!slot)
            break;
        auto p = reinterpret_cast<volatile LONG*>(slot);
        if (static_cast<uintptr_t>(*p) != def.address)
        {
            AVP_LOG(kLog, "SKIP slot %08X for %s: holds %08X, expected %08X", static_cast<unsigned>(slot), def.name,
                    static_cast<unsigned>(*p), static_cast<unsigned>(def.address));
            continue;
        }
        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(slot), sizeof(LONG), PAGE_READWRITE, &oldProtect))
        {
            AVP_LOG(kLog, "VirtualProtect failed on slot %08X for %s (%lu)", static_cast<unsigned>(slot), def.name,
                    GetLastError());
            continue;
        }
        InterlockedExchange(p, static_cast<LONG>(reinterpret_cast<uintptr_t>(stub)));
        VirtualProtect(reinterpret_cast<void*>(slot), sizeof(LONG), oldProtect, &oldProtect);
        AVP_LOG(kLog, "slot hook %s: %08X now -> stub %p (original %08X)", def.name, static_cast<unsigned>(slot), stub,
                static_cast<unsigned>(def.address));
        ++patched;
    }
    return patched > 0;
}

} // namespace

int InstallTraceHooks()
{
    auto stubs = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kTraceCount * kStubSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stubs)
    {
        AVP_LOG(kLog, "VirtualAlloc for %d stubs failed (%lu)", kTraceCount, GetLastError());
        return 0;
    }

    int installed = 0;
    for (int i = 0; i < kTraceCount; ++i)
    {
        const TraceDef& def = kTraces[i];
        if ((def.group & dev::kTraceGroups) == 0)
            continue;
        uint8_t* stub = stubs + i * kStubSize;
        BuildStub(stub, static_cast<uint32_t>(i));
        g_stubs[i] = stub;
        if (def.shadow)
        {
            g_trampolines[i] = reinterpret_cast<void*>(def.address); // the stub continues into the original
            g_state[i].installed = true;
            ++installed;
            AVP_LOG(kLog, "shadow stub %s ready at %p (original %08X)", def.name, stub,
                    static_cast<unsigned>(def.address));
            continue;
        }
        if (IsSlotHook(def))
        {
            if (InstallSlotHook(i, stub))
            {
                g_state[i].installed = true;
                ++installed;
            }
            continue;
        }
        void* target = reinterpret_cast<void*>(def.address);
        MH_STATUS status = MH_CreateHook(target, stub, &g_trampolines[i]);
        if (status == MH_OK)
            status = MH_EnableHook(target);
        if (status != MH_OK)
        {
            AVP_LOG(kLog, "FAILED %s @ %08X: %s", def.name, static_cast<unsigned>(def.address), MH_StatusToString(status));
            continue;
        }
        g_state[i].installed = true;
        ++installed;
    }
    AVP_LOG(kLog, "trace hooks installed: %d of %d (groups 0x%08X)", installed, kTraceCount, dev::kTraceGroups);
    return installed;
}

bool SetTracePre(uintptr_t address, TracePreFn fn)
{
    for (int i = 0; i < kTraceCount; ++i)
    {
        if (kTraces[i].address == address && g_state[i].installed)
        {
            g_state[i].pre.store(fn, std::memory_order_release);
            return true;
        }
    }
    return false;
}

void* TraceShadowStub(uintptr_t functionAddress)
{
    for (int i = 0; i < kTraceCount; ++i)
    {
        if (kTraces[i].shadow && kTraces[i].address == functionAddress && g_state[i].installed)
            return g_stubs[i];
    }
    return nullptr;
}

void StartTraceSummaryThread()
{
    std::thread(SummaryLoop).detach();
}

} // namespace avp
