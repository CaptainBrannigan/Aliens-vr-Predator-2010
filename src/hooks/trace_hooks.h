#pragma once
// Log-only hooks, many at once, without per-function prototypes.
//
// Each traced address gets a small generated x86 stub as its MinHook detour. The stub saves every register, the
// flags and the x87/SSE state (fxsave), calls OnTraceHit(index, ctx), restores everything and jumps to the original
// code -- so it works for any calling convention (cdecl/stdcall/thiscall/fastcall/custom LTCG register conventions)
// and never changes what the hooked function sees.
//
// Per hook it records: call count, call rate, which threads call it, the distinct return addresses (= callers),
// and, throttled, a register + stack-argument dump with an optional typed memory dump (floats behind an argument
// pointer, fields of ECX, ...). Output: avpvr_trace.log in the game folder.
//
// A hook can also carry a "pre" callback run before the original function -- used by experiments (two-view test)
// so they don't need hook plumbing of their own.

#include <cstdint>

namespace avp {

// Register context captured by the stub: pushad order, then EFLAGS, then the hooked call's return address and its
// stack arguments.
struct TraceCtx
{
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eflags;
    uint32_t ret;
    uint32_t args[1]; // args[0] = first stack argument; read further with Arg(i)

    uint32_t Arg(int i) const { return (&args[0])[i]; }
};

using TracePreFn = void (*)(const TraceCtx*);

// Installs every trace hook in the enabled groups (dev::kTraceGroups). MH_Initialize must already have run.
// Returns the number of hooks installed.
int InstallTraceHooks();

// Attaches a pre-callback to the traced function at `address` (must be in the trace table and installed).
bool SetTracePre(uintptr_t address, TracePreFn fn);

// Starts the background thread that writes the periodic per-hook summaries.
void StartTraceSummaryThread();

} // namespace avp
