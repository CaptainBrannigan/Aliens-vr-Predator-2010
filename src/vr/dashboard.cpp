#include "dashboard.h"

#include "openvr_session.h"
#include "stereo.h"
#include "../core/log.h"
#include "../core/settings.h"

#include <windows.h>
#include <openvr.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace avp::dashboard {
namespace {

constexpr const char* kLog = "avpvr_dashboard.log";

constexpr int kWidth = 1800;
constexpr int kHeight = 900;
constexpr int kThumbSize = 256;

// Layout.
constexpr int kTitleHeight = 80;
constexpr int kColumnCount = 2;
constexpr int kColumnX[kColumnCount] = {30, 915};
constexpr int kColumnWidth = 855;
constexpr int kHeaderHeight = 44;
constexpr int kRowHeight = 36;
constexpr int kButtonWidth = 54;
constexpr int kButtonGap = 6;
constexpr int kToggleWidth = 120;
constexpr int kChoiceTextWidth = 330;

struct HitBox
{
    RECT rect;
    int index; // menu index, or one of the action ids below
    int steps;
};
constexpr int kRecenterButton = -100;

::vr::IVROverlay* g_overlay = nullptr;
::vr::VROverlayHandle_t g_main = ::vr::k_ulOverlayHandleInvalid;
::vr::VROverlayHandle_t g_thumb = ::vr::k_ulOverlayHandleInvalid;

// Overlay thread only.
std::vector<HitBox> g_hits;
int g_hover = -1;

int ColumnForGroup(const char* group)
{
    return strcmp(group, "Controls") == 0 || strcmp(group, "Debug") == 0 ? 1 : 0;
}

class Canvas
{
public:
    Canvas(int width, int height) : m_width(width), m_height(height)
    {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        m_dc = CreateCompatibleDC(nullptr);
        m_bitmap = CreateDIBSection(m_dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&m_bits), nullptr, 0);
        if (m_dc && m_bitmap)
            m_old = SelectObject(m_dc, m_bitmap);
        SetBkMode(m_dc, TRANSPARENT);
        m_rgba.resize(static_cast<size_t>(width) * height * 4);
    }
    bool Ok() const { return m_dc && m_bitmap && m_bits; }

    void Fill(const RECT& r, COLORREF color)
    {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(m_dc, &r, brush);
        DeleteObject(brush);
    }

    void Text(const char* text, RECT r, HFONT font, COLORREF color, UINT format)
    {
        HGDIOBJ old = SelectObject(m_dc, font);
        SetTextColor(m_dc, color);
        DrawTextA(m_dc, text, -1, &r, format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(m_dc, old);
    }

    // GDI writes BGRX; SetOverlayRaw wants RGBA.
    void Upload(::vr::VROverlayHandle_t handle)
    {
        GdiFlush();
        const uint8_t* src = m_bits;
        uint8_t* dst = m_rgba.data();
        for (size_t i = 0, n = static_cast<size_t>(m_width) * m_height; i < n; ++i, src += 4, dst += 4)
        {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 255;
        }
        ::vr::EVROverlayError err;
        {
            std::lock_guard<std::mutex> lock(vr::OverlayMutex());
            err = g_overlay->SetOverlayRaw(handle, m_rgba.data(), m_width, m_height, 4);
        }
        static int errLines = 0;
        if (err != ::vr::VROverlayError_None && errLines++ < 10)
            AVP_LOG(kLog, "SetOverlayRaw err=%d", err);
    }

private:
    int m_width, m_height;
    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_old = nullptr;
    uint8_t* m_bits = nullptr;
    std::vector<uint8_t> m_rgba;
};

HFONT MakeFont(int height, int weight)
{
    return CreateFontA(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Segoe UI");
}

const COLORREF kBackground = RGB(20, 24, 22);
const COLORREF kRowA = RGB(28, 34, 31);
const COLORREF kRowB = RGB(34, 41, 37);
const COLORREF kTextColor = RGB(228, 234, 230);
const COLORREF kDimText = RGB(146, 158, 150);
const COLORREF kButton = RGB(56, 66, 60);
const COLORREF kButtonHover = RGB(70, 130, 96);
const COLORREF kToggleOn = RGB(46, 125, 72);
const COLORREF kAccent = RGB(120, 210, 140);

// g_hover indexes g_hits, rebuilt in the same order every draw, so the hovered button keeps its highlight.
void DrawButton(Canvas& c, const RECT& r, const char* text, HFONT font, COLORREF color, int index, int steps)
{
    const int myIndex = static_cast<int>(g_hits.size());
    g_hits.push_back(HitBox{r, index, steps});
    c.Fill(r, myIndex == g_hover ? kButtonHover : color);
    c.Text(text, r, font, kTextColor, DT_CENTER);
}

void DrawMenu(Canvas& c)
{
    static HFONT titleFont = MakeFont(36, FW_BOLD);
    static HFONT headerFont = MakeFont(27, FW_BOLD);
    static HFONT rowFont = MakeFont(23, FW_NORMAL);
    static HFONT smallFont = MakeFont(19, FW_NORMAL);

    g_hits.clear();
    RECT all{0, 0, kWidth, kHeight};
    c.Fill(all, kBackground);

    RECT title{kColumnX[0], 0, kWidth - 30, kTitleHeight};
    c.Text("AvP VR settings", title, titleFont, kAccent, DT_LEFT);
    RECT hint{kColumnX[0] + 300, 0, kWidth - 280, kTitleHeight};
    c.Text("Changes apply and save immediately", hint, smallFont, kDimText, DT_LEFT);
    RECT recenter{kWidth - 30 - 220, 18, kWidth - 30, kTitleHeight - 14};
    DrawButton(c, recenter, "Recenter view", rowFont, kButton, kRecenterButton, 0);

    int y[kColumnCount] = {kTitleHeight, kTitleHeight};
    const char* lastGroup[kColumnCount] = {nullptr, nullptr};
    int parity[kColumnCount] = {0, 0};
    for (int i = 0, n = settings::MenuCount(); i < n; ++i)
    {
        settings::MenuInfo info{};
        if (!settings::GetMenu(i, &info))
            continue;
        const int col = ColumnForGroup(info.group);
        const int x = kColumnX[col];
        if (!lastGroup[col] || strcmp(lastGroup[col], info.group) != 0)
        {
            RECT header{x, y[col] + 6, x + kColumnWidth, y[col] + kHeaderHeight};
            c.Text(info.group, header, headerFont, kAccent, DT_LEFT);
            y[col] += kHeaderHeight;
            lastGroup[col] = info.group;
            parity[col] = 0;
        }
        RECT row{x, y[col], x + kColumnWidth, y[col] + kRowHeight};
        y[col] += kRowHeight;
        if (row.bottom > kHeight)
            continue;
        c.Fill(row, (parity[col]++ & 1) ? kRowB : kRowA);
        const int right = x + kColumnWidth - 8;
        const int top = row.top + 3, bottom = row.bottom - 3;
        if (info.isToggle)
        {
            RECT label{x + 12, row.top, right - kToggleWidth - 12, row.bottom};
            c.Text(info.label, label, rowFont, kTextColor, DT_LEFT | DT_END_ELLIPSIS);
            RECT button{right - kToggleWidth, top, right, bottom};
            const bool on = info.value != 0.0f;
            DrawButton(c, button, on ? "ON" : "OFF", rowFont, on ? kToggleOn : kButton, i, 1);
        }
        else if (info.isChoice)
        {
            // label [<] choice [>]
            RECT next{right - kButtonWidth, top, right, bottom};
            RECT prev{next.left - kButtonGap - kChoiceTextWidth - kButtonGap - kButtonWidth, top,
                      next.left - kButtonGap - kChoiceTextWidth - kButtonGap, bottom};
            RECT valueRect{prev.right + kButtonGap, row.top, next.left - kButtonGap, row.bottom};
            RECT label{x + 12, row.top, prev.left - 12, row.bottom};
            c.Text(info.label, label, rowFont, kTextColor, DT_LEFT | DT_END_ELLIPSIS);
            DrawButton(c, prev, "<", rowFont, kButton, i, -1);
            c.Text(info.text, valueRect, rowFont, kTextColor, DT_CENTER | DT_END_ELLIPSIS);
            DrawButton(c, next, ">", rowFont, kButton, i, 1);
        }
        else
        {
            // label  value [--] [-] [+] [++]; the doubled buttons move ten steps.
            const char* labels[4] = {"--", "-", "+", "++"};
            const int steps[4] = {-10, -1, 1, 10};
            const int buttonsLeft = right - 4 * kButtonWidth - 3 * kButtonGap;
            char value[48];
            snprintf(value, sizeof(value), "%.*f %s", info.decimals, info.value, info.unit);
            RECT valueRect{buttonsLeft - 180, row.top, buttonsLeft - 14, row.bottom};
            RECT label{x + 12, row.top, valueRect.left - 8, row.bottom};
            c.Text(info.label, label, rowFont, kTextColor, DT_LEFT | DT_END_ELLIPSIS);
            c.Text(value, valueRect, rowFont, kTextColor, DT_RIGHT);
            for (int b = 0; b < 4; ++b)
            {
                const int left = buttonsLeft + b * (kButtonWidth + kButtonGap);
                RECT button{left, top, left + kButtonWidth, bottom};
                DrawButton(c, button, labels[b], rowFont, kButton, i, steps[b]);
            }
        }
    }
}

void DrawThumbnail(Canvas& c)
{
    HFONT font = MakeFont(64, FW_BOLD);
    RECT all{0, 0, kThumbSize, kThumbSize};
    c.Fill(all, kBackground);
    RECT top{0, 40, kThumbSize, 130};
    RECT bottom{0, 125, kThumbSize, 215};
    c.Text("AvP", top, font, kAccent, DT_CENTER);
    c.Text("VR", bottom, font, kTextColor, DT_CENTER);
    DeleteObject(font);
}

int HitTest(float px, float py)
{
    for (int i = 0; i < static_cast<int>(g_hits.size()); ++i)
    {
        const RECT& r = g_hits[i].rect;
        if (px >= r.left && px < r.right && py >= r.top && py < r.bottom)
            return i;
    }
    return -1;
}

void ThreadProc()
{
    // Leaked on purpose: no exit-time destructors on a thread the game's exit kills.
    auto* menu = new Canvas(kWidth, kHeight);
    auto* thumb = new Canvas(kThumbSize, kThumbSize);
    if (!menu->Ok() || !thumb->Ok())
    {
        AVP_LOG(kLog, "GDI canvas creation FAILED -- no menu");
        return;
    }
    DrawThumbnail(*thumb);
    thumb->Upload(g_thumb);

    bool dirty = true;
    ULONGLONG lastDraw = 0;
    for (;;)
    {
        ::vr::VREvent_t ev{};
        for (;;)
        {
            bool got;
            {
                std::lock_guard<std::mutex> lock(vr::OverlayMutex());
                got = g_overlay->PollNextOverlayEvent(g_main, &ev, sizeof(ev));
            }
            if (!got)
                break;
            if (ev.eventType == ::vr::VREvent_MouseMove || ev.eventType == ::vr::VREvent_MouseButtonDown)
            {
                const float px = ev.data.mouse.x;
                const float py = static_cast<float>(kHeight) - ev.data.mouse.y; // events are bottom-left origin
                const int hit = HitTest(px, py);
                if (hit != g_hover)
                {
                    g_hover = hit;
                    dirty = true;
                }
                if (ev.eventType == ::vr::VREvent_MouseButtonDown && hit >= 0)
                {
                    if (g_hits[hit].index == kRecenterButton)
                        stereo::RequestRecenter();
                    else
                        settings::AdjustMenu(g_hits[hit].index, g_hits[hit].steps);
                    dirty = true;
                }
            }
            ev = ::vr::VREvent_t{};
        }

        bool visible;
        {
            std::lock_guard<std::mutex> lock(vr::OverlayMutex());
            visible = g_overlay->IsOverlayVisible(g_main);
        }
        const ULONGLONG now = GetTickCount64();
        if (visible && (dirty || now - lastDraw >= 1000))
        {
            DrawMenu(*menu);
            menu->Upload(g_main);
            dirty = false;
            lastDraw = now;
        }
        Sleep(visible ? 15 : 100);
    }
}

} // namespace

void Start(::vr::IVROverlay* overlay)
{
    if (!overlay || g_overlay)
        return;
    ::vr::EVROverlayError err;
    {
        std::lock_guard<std::mutex> lock(vr::OverlayMutex());
        err = overlay->CreateDashboardOverlay("avpvr.settings", "AvP VR", &g_main, &g_thumb);
        if (err == ::vr::VROverlayError_None)
        {
            const ::vr::HmdVector2_t scale{static_cast<float>(kWidth), static_cast<float>(kHeight)};
            overlay->SetOverlayWidthInMeters(g_main, 2.5f);
            overlay->SetOverlayInputMethod(g_main, ::vr::VROverlayInputMethod_Mouse);
            overlay->SetOverlayMouseScale(g_main, &scale);
        }
    }
    AVP_LOG(kLog, "CreateDashboardOverlay err=%d main=%llu thumb=%llu", err, static_cast<unsigned long long>(g_main),
            static_cast<unsigned long long>(g_thumb));
    if (err != ::vr::VROverlayError_None)
        return;
    g_overlay = overlay;
    std::thread(ThreadProc).detach();
    AVP_LOG(kLog, "dashboard tab ready (%d settings)", settings::MenuCount());
}

} // namespace avp::dashboard
