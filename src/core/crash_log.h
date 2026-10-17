#pragma once
// Crash forensics: a first-chance vectored exception handler that writes fatal-looking exceptions (access violation,
// illegal instruction, stack overflow, divide by zero, ...) to avpvr_crash.log -- faulting address with module+offset,
// registers, and code addresses found on the stack. Writes with WriteFile on a handle opened up front, no locks, so it
// works even if the crash happened inside the mod's own logger. AvP has its own crash handling that exits silently
// (no Windows Error Reporting entry), which is why this exists.
//
// First-chance means exceptions the game handles itself are logged too; the log is capped and each entry says so.

namespace avp {

void InstallCrashLogger();

// Hooks kernel32 ExitProcess and TerminateProcess(self) to log who ends the process, with a stack scan -- AvP's own
// crash handling exits without a Windows Error Reporting entry. MH_Initialize must have run.
void InstallExitHooks();

// Called from DllMain on process detach: records a normal ExitProcess (a TerminateProcess never gets here).
void LogProcessExit();

} // namespace avp
