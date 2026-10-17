#pragma once
// File logger shared by the whole mod (ported from the MOHW mod's sdk/logging.h, then changed to raw Win32 I/O).
//
// One HANDLE per log file for the process lifetime, written with WriteFile -- deliberately no CRT FILE* streams:
// the game's exit kills our background threads, possibly mid-write, and the static CRT's shutdown would then wait
// forever on a stream lock held by a dead thread (first live run, 2026-10-16, ended in a process stuck at exit).
// WriteFile with no buffering also means every line is on disk the moment the call returns.
// Files live next to the DLL (the game folder). Each launch truncates a file on first use; the previous session's
// copy is kept as <name>.previous.log. Opened with FILE_SHARE_READ so logs can be read live.

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <mutex>

namespace avp {

inline std::string ModuleDirectory()
{
    char modulePath[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ModuleDirectory), &self);
    GetModuleFileNameA(self, modulePath, MAX_PATH);
    std::string path(modulePath);
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

inline std::unordered_map<std::string, HANDLE>& LogFileHandles()
{
    static auto* handles = new std::unordered_map<std::string, HANDLE>(); // leaked on purpose: no exit-time destructor
    return *handles;
}

inline std::mutex& LogMutex()
{
    static auto* m = new std::mutex(); // leaked on purpose, see above
    return *m;
}

inline HANDLE OpenLogFile(const char* fileName)
{
    std::string path = ModuleDirectory() + fileName;
    std::string previous = path;
    size_t dot = previous.find_last_of('.');
    previous = (dot == std::string::npos ? previous : previous.substr(0, dot)) + ".previous.log";
    MoveFileExA(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    return CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

inline void LogV(const char* fileName, const char* fmt, va_list args)
{
    char line[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int used = _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond,
                           st.wMilliseconds);
    if (used < 0)
        used = 0;
    int body = _vsnprintf_s(line + used, sizeof(line) - used - 2, _TRUNCATE, fmt, args);
    used = body < 0 ? static_cast<int>(strlen(line)) : used + body;
    line[used++] = '\r';
    line[used++] = '\n';

    std::lock_guard<std::mutex> lock(LogMutex());
    auto& handles = LogFileHandles();
    auto it = handles.find(fileName);
    HANDLE h = it != handles.end() ? it->second : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE)
    {
        h = OpenLogFile(fileName);
        if (h == INVALID_HANDLE_VALUE)
            return;
        handles[fileName] = h;
    }
    DWORD written = 0;
    WriteFile(h, line, static_cast<DWORD>(used), &written, nullptr);
}

inline void Log(const char* fileName, const char* fmt, ...)
{
    DWORD lastError = GetLastError(); // logging from inside hooks must not disturb the game's error state
    va_list args;
    va_start(args, fmt);
    LogV(fileName, fmt, args);
    va_end(args);
    SetLastError(lastError);
}

} // namespace avp

#define AVP_LOG(file, ...) ::avp::Log(file, __VA_ARGS__)
