#include "settings.h"

#include "log.h"
#include "../hooks/hud_passes.h"


#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace avp::settings {
namespace {

constexpr const char* kLog = "avpvr_settings.log";
constexpr const char* kFileName = "avpvr_settings.ini";

enum class Kind
{
    Toggle,
    Number,
    Choice,
};

struct Entry
{
    const char* key; // ini name
    const char* group;
    const char* label;
    Kind kind;
    float defaultValue;
    float minValue, maxValue, step; // Number; Choice: 0 .. count - 1
    int decimals;
    const char* unit;
    const char* (*choiceLabel)(int); // Choice only
};


// "Debug: hide pass" choices: 0 = off, i = kHudPasses[i - 1].
// "Debug: hide pass" choices: 0 = off, i = kHudPasses[i - 1], kHudPassCount + 1 = unlisted draws.
const char* HidePassLabel(int i) { return i <= 0 ? "off" : HudPassLabel(i - 1); }


// Order = display order; entries of a group must be contiguous.
const Entry kEntries[] = {
    {"IpdScale", "View", "Eye distance (IPD) scale", Kind::Number, 1.0f, 0.5f, 2.0f, 0.02f, 2, "x", nullptr},
    {"HudPanel", "HUD", "HUD in a panel (off = full view)", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"HudWidthDeg", "HUD", "HUD size (width)", Kind::Number, 60.0f, 20.0f, 110.0f, 1.0f, 0, "deg", nullptr},
    {"HudDistance", "HUD", "HUD distance", Kind::Number, 2.5f, 0.5f, 20.0f, 0.1f, 1, "m", nullptr},
    {"HudHeightDeg", "HUD", "HUD height (+ up)", Kind::Number, 0.0f, -30.0f, 30.0f, 0.5f, 1, "deg", nullptr},
    {"HideDuplicateTracker", "HUD", "Hide the duplicate motion tracker", Kind::Toggle, 1.0f, 0, 0, 0, 0, "", nullptr},
    {"StickFullDeflection", "Controls", "Stick counts as full at", Kind::Number, 0.95f, 0.5f, 1.0f, 0.01f, 2, "",
     nullptr},
    {"TurnSpeed", "Controls", "Look speed (right stick)", Kind::Number, 1.0f, 0.2f, 3.0f, 0.05f, 2, "x", nullptr},
    {"DebugHidePass", "Debug", "Hide pass", Kind::Choice, 0.0f, 0.0f, static_cast<float>(kHudPassCount + 1), 1.0f, 0, "",
     &HidePassLabel},
    {"DebugHideDraw", "Debug", "Hide only draw # of it (0 = all)", Kind::Number, 0.0f, 0.0f, 999.0f, 1.0f, 0, "",
     nullptr},


};
constexpr int kCount = static_cast<int>(sizeof(kEntries) / sizeof(kEntries[0]));

enum Index
{
    kIpdScale,
    kHudPanel,
    kHudWidthDeg,
    kHudDistance,
    kHudHeightDeg,
    kHideDuplicateTracker,
    kStickFullDeflection,
    kTurnSpeed,
    kDebugHidePass,
    kDebugHideDraw,
};

std::atomic<float> g_values[kCount];
std::mutex g_fileMutex;

std::string FilePath() { return ModuleDirectory() + kFileName; }

float Clamp(const Entry& e, float v)
{
    if (e.kind == Kind::Toggle)
        return v != 0.0f ? 1.0f : 0.0f;
    if (v < e.minValue)
        v = e.minValue;
    if (v > e.maxValue)
        v = e.maxValue;
    return e.kind == Kind::Choice ? std::floor(v + 0.5f) : v;
}

void WriteFileLocked()
{
    std::string text = "; AvP VR settings -- change them in the SteamVR dashboard (AvP VR tab); this file is rewritten on "
                       "every change.\r\n";
    char line[160];
    for (int i = 0; i < kCount; ++i)
    {
        const Entry& e = kEntries[i];
        const float v = g_values[i].load(std::memory_order_relaxed);
        if (e.kind == Kind::Toggle)
            snprintf(line, sizeof(line), "%s=%d\r\n", e.key, v != 0.0f ? 1 : 0);
        else if (e.kind == Kind::Choice)
            snprintf(line, sizeof(line), "%s=%d\r\n", e.key, static_cast<int>(v));
        else
            snprintf(line, sizeof(line), "%s=%.*f\r\n", e.key, e.decimals + 2, v);
        text += line;
    }
    // Raw Win32 like the logger (no CRT streams that could be mid-write at exit).
    HANDLE h = CreateFileA(FilePath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        AVP_LOG(kLog, "could not write %s (%lu)", FilePath().c_str(), GetLastError());
        return;
    }
    DWORD written = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
}

} // namespace

void Load()
{
    std::lock_guard<std::mutex> lock(g_fileMutex);
    for (int i = 0; i < kCount; ++i)
        g_values[i].store(kEntries[i].defaultValue, std::memory_order_relaxed);

    std::string content;
    HANDLE h = CreateFileA(FilePath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0)
            content.append(buf, got);
        CloseHandle(h);
    }
    int loaded = 0;
    size_t pos = 0;
    while (pos < content.size())
    {
        size_t end = content.find('\n', pos);
        std::string line = content.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? content.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == ';' || eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        for (int i = 0; i < kCount; ++i)
            if (key == kEntries[i].key)
            {
                g_values[i].store(Clamp(kEntries[i], static_cast<float>(atof(line.c_str() + eq + 1))),
                                  std::memory_order_relaxed);
                ++loaded;
            }
    }
    AVP_LOG(kLog, "%s: %d of %d values read%s", FilePath().c_str(), loaded, kCount,
            content.empty() ? " (no file: defaults)" : "");
    for (int i = 0; i < kCount; ++i)
        AVP_LOG(kLog, "  %s = %.3f", kEntries[i].key, g_values[i].load(std::memory_order_relaxed));
    // Rewrite: adds new settings, drops obsolete ones, normalizes the format.
    WriteFileLocked();
}

float IpdScale() { return g_values[kIpdScale].load(std::memory_order_relaxed); }
bool HudPanel() { return g_values[kHudPanel].load(std::memory_order_relaxed) != 0.0f; }
float HudWidthDeg() { return g_values[kHudWidthDeg].load(std::memory_order_relaxed); }
float HudDistance() { return g_values[kHudDistance].load(std::memory_order_relaxed); }
float HudHeightDeg() { return g_values[kHudHeightDeg].load(std::memory_order_relaxed); }


float StickFullDeflection() { return g_values[kStickFullDeflection].load(std::memory_order_relaxed); }
float TurnSpeed() { return g_values[kTurnSpeed].load(std::memory_order_relaxed); }
int DebugHidePass() { return static_cast<int>(g_values[kDebugHidePass].load(std::memory_order_relaxed)) - 1; }
bool HideDuplicateTracker() { return g_values[kHideDuplicateTracker].load(std::memory_order_relaxed) != 0.0f; }
int DebugHideDraw() { return static_cast<int>(g_values[kDebugHideDraw].load(std::memory_order_relaxed)); }

int MenuCount() { return kCount; }

bool GetMenu(int index, MenuInfo* out)
{
    if (index < 0 || index >= kCount || !out)
        return false;
    const Entry& e = kEntries[index];
    const float v = g_values[index].load(std::memory_order_relaxed);
    out->group = e.group;
    out->label = e.label;
    out->isToggle = e.kind == Kind::Toggle;
    out->isChoice = e.kind == Kind::Choice;
    out->value = v;
    out->decimals = e.decimals;
    out->unit = e.unit;
    out->text = e.kind == Kind::Choice && e.choiceLabel ? e.choiceLabel(static_cast<int>(v)) : "";
    return true;
}

void AdjustMenu(int index, int steps)
{
    if (index < 0 || index >= kCount || steps == 0)
        return;
    const Entry& e = kEntries[index];
    float v = g_values[index].load(std::memory_order_relaxed);
    if (e.kind == Kind::Toggle)
        v = v != 0.0f ? 0.0f : 1.0f;
    else if (e.kind == Kind::Choice)
    {
        const int n = static_cast<int>(e.maxValue) + 1;
        v = static_cast<float>(((static_cast<int>(v) + (steps > 0 ? 1 : -1)) % n + n) % n);
    }
    else
    {
        v += e.step * static_cast<float>(steps);
        v = std::round(v / e.step) * e.step; // stay on the step grid
        v = Clamp(e, v);
    }
    g_values[index].store(v, std::memory_order_relaxed);
    AVP_LOG(kLog, "%s -> %.3f", e.key, v);
    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteFileLocked();
}

} // namespace avp::settings
