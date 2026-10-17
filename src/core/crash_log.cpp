#include "crash_log.h"

#include "log.h"

#include <MinHook.h>
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace avp {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
std::atomic<int> g_logged{0};
constexpr int kMaxEntries = 20;
constexpr int kStackScanDwords = 96;

void WriteRaw(const char* text)
{
    if (g_file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(g_file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
}

void WriteF(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    WriteRaw(buf);
}

// "module+offset" for an address, or "?" when it isn't inside a loaded module.
void DescribeAddress(uintptr_t address, char* out, size_t outSize)
{
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) && module)
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        _snprintf_s(out, outSize, _TRUNCATE, "%s+0x%X", name,
                    static_cast<unsigned>(address - reinterpret_cast<uintptr_t>(module)));
        return;
    }
    _snprintf_s(out, outSize, _TRUNCATE, "?");
}

bool IsExecutable(uintptr_t address)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec) != 0;
}

bool SafeReadDword(uintptr_t address, uint32_t* out)
{
    __try
    {
        *out = *reinterpret_cast<const uint32_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool IsFatalLooking(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case 0xC0000409: // stack buffer overrun (when raised as an exception)
        return true;
    default:
        return false;
    }
}

void ScanStack(uintptr_t esp)
{
    WriteRaw("code addresses on the stack (likely return addresses):\r\n");
    int shown = 0;
    for (int i = 0; i < kStackScanDwords && shown < 24; ++i)
    {
        uint32_t value = 0;
        if (!SafeReadDword(esp + i * 4, &value))
            break;
        if (value < 0x10000 || !IsExecutable(value))
            continue;
        char desc[160];
        DescribeAddress(value, desc, sizeof(desc));
        WriteF("  [esp+%03X] %08X %s\r\n", i * 4, value, desc);
        ++shown;
    }
}

// ---- Who ends the process ----

using PFN_ExitProcess = void(WINAPI*)(UINT);
using PFN_TerminateProcess = BOOL(WINAPI*)(HANDLE, UINT);
PFN_ExitProcess g_realExitProcess = nullptr;
PFN_TerminateProcess g_realTerminateProcess = nullptr;

void LogExitCall(const char* what, UINT code, void* returnAddress, void* addressOfReturnAddress)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char where[160];
    DescribeAddress(reinterpret_cast<uintptr_t>(returnAddress), where, sizeof(where));
    WriteF("\r\n==== %s(%u / 0x%X) at %02d:%02d:%02d.%03d tid %lu, called from %08X (%s) ====\r\n", what, code, code,
           st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(),
           static_cast<unsigned>(reinterpret_cast<uintptr_t>(returnAddress)), where);
    ScanStack(reinterpret_cast<uintptr_t>(addressOfReturnAddress));
    if (g_file != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_file);
}

void WINAPI Hook_ExitProcess(UINT code)
{
    LogExitCall("ExitProcess", code, _ReturnAddress(), _AddressOfReturnAddress());
    g_realExitProcess(code);
}

BOOL WINAPI Hook_TerminateProcess(HANDLE process, UINT code)
{
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
        LogExitCall("TerminateProcess(self)", code, _ReturnAddress(), _AddressOfReturnAddress());
    return g_realTerminateProcess(process, code);
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (!IsFatalLooking(rec->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    // The mod's own guarded reads (__try in the heap scanner, snapshot, trace dumps) fault on memory freed under them
    // and handle it themselves -- not crashes, don't log them.
    HMODULE faultModule = nullptr, selfModule = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(rec->ExceptionAddress), &faultModule);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&OnException), &selfModule);
    if (faultModule && faultModule == selfModule)
        return EXCEPTION_CONTINUE_SEARCH;
    const int n = g_logged.fetch_add(1) + 1;
    if (n > kMaxEntries)
        return EXCEPTION_CONTINUE_SEARCH;

    const DWORD lastError = GetLastError();
    const CONTEXT* c = info->ContextRecord;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char where[160];
    DescribeAddress(reinterpret_cast<uintptr_t>(rec->ExceptionAddress), where, sizeof(where));

    WriteF("\r\n==== exception %d (first chance) at %02d:%02d:%02d.%03d tid %lu ====\r\n", n, st.wHour, st.wMinute,
           st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    WriteF("code %08lX at %08X (%s)\r\n", rec->ExceptionCode,
           static_cast<unsigned>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)), where);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        WriteF("access violation %s address %08X\r\n",
               rec->ExceptionInformation[0] == 0 ? "reading" : rec->ExceptionInformation[0] == 1 ? "writing" : "executing",
               static_cast<unsigned>(rec->ExceptionInformation[1]));
    WriteF("eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX\r\nebp=%08lX esp=%08lX eip=%08lX efl=%08lX\r\n",
           c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip, c->EFlags);

    ScanStack(c->Esp);
    if (n == kMaxEntries)
        WriteRaw("(entry cap reached -- further exceptions not logged)\r\n");
    FlushFileBuffers(g_file);
    SetLastError(lastError);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void InstallCrashLogger()
{
    const std::string path = ModuleDirectory() + "avpvr_crash.log";
    const std::string previous = ModuleDirectory() + "avpvr_crash.previous.log";
    MoveFileExA(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    WriteRaw("avpvr crash log -- first-chance fatal-looking exceptions (the game may handle some itself)\r\n");
    AddVectoredExceptionHandler(1, &OnException);
}

void InstallExitHooks()
{
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    void* exitProcess = kernel32 ? reinterpret_cast<void*>(GetProcAddress(kernel32, "ExitProcess")) : nullptr;
    void* terminateProcess = kernel32 ? reinterpret_cast<void*>(GetProcAddress(kernel32, "TerminateProcess")) : nullptr;
    MH_STATUS a = MH_UNKNOWN, b = MH_UNKNOWN;
    if (exitProcess && (a = MH_CreateHook(exitProcess, reinterpret_cast<void*>(&Hook_ExitProcess),
                                          reinterpret_cast<void**>(&g_realExitProcess))) == MH_OK)
        a = MH_EnableHook(exitProcess);
    if (terminateProcess && (b = MH_CreateHook(terminateProcess, reinterpret_cast<void*>(&Hook_TerminateProcess),
                                               reinterpret_cast<void**>(&g_realTerminateProcess))) == MH_OK)
        b = MH_EnableHook(terminateProcess);
    WriteF("exit hooks: ExitProcess %s, TerminateProcess %s\r\n", MH_StatusToString(a), MH_StatusToString(b));
}

void LogProcessExit()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    WriteF("\r\nprocess exiting normally (DLL_PROCESS_DETACH) at %02d:%02d:%02d.%03d, %d exception(s) logged\r\n",
           st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, g_logged.load());
    if (g_file != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_file);
}

} // namespace avp
