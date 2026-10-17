#pragma once
// File logger shared by the whole mod (ported from the MOHW mod's sdk/logging.h).
//
// One FILE* per log file for the process lifetime, guarded by a mutex, fflush after every line so a crash loses
// nothing. Files live next to the DLL (the game folder). Each launch truncates a file on first use; the previous
// session's copy is kept as <name>.previous.log, so a crash followed by a relaunch doesn't wipe the evidence.
// Opened with FILE_SHARE_READ so logs can be read live while the game runs.

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdarg>
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

inline std::unordered_map<std::string, FILE*>& LogFileHandles()
{
    static std::unordered_map<std::string, FILE*> handles;
    return handles;
}

inline std::mutex& LogMutex()
{
    static std::mutex m;
    return m;
}

inline FILE* OpenLogFile(const char* fileName)
{
    std::string path = ModuleDirectory() + fileName;
    std::string previous = path;
    size_t dot = previous.find_last_of('.');
    previous = (dot == std::string::npos ? previous : previous.substr(0, dot)) + ".previous.log";
    MoveFileExA(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);

    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return nullptr;
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_APPEND | _O_TEXT);
    if (fd == -1)
    {
        CloseHandle(file);
        return nullptr;
    }
    FILE* f = _fdopen(fd, "a");
    if (!f)
        _close(fd);
    return f;
}

inline void LogV(const char* fileName, const char* fmt, va_list args)
{
    std::lock_guard<std::mutex> lock(LogMutex());
    auto& handles = LogFileHandles();
    auto it = handles.find(fileName);
    FILE* f = it != handles.end() ? it->second : nullptr;
    if (!f)
    {
        f = OpenLogFile(fileName);
        if (!f)
            return;
        handles[fileName] = f;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    vfprintf(f, fmt, args);
    fputc('\n', f);
    fflush(f);
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
