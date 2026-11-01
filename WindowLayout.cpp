#include "WindowLayout.h"
#include <algorithm>
#include <string>

namespace ui {
RECT FitToWorkArea(RECT requested, const RECT& work) {
    const LONG width = (std::min)((std::max)(1L, requested.right - requested.left), work.right - work.left);
    const LONG height = (std::min)((std::max)(1L, requested.bottom - requested.top), work.bottom - work.top);
    requested.left = (std::max)(work.left, (std::min)(requested.left, work.right - width));
    requested.top = (std::max)(work.top, (std::min)(requested.top, work.bottom - height));
    requested.right = requested.left + width;
    requested.bottom = requested.top + height;
    return requested;
}
RECT WorkAreaFor(RECT requested) {
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromRect(&requested, MONITOR_DEFAULTTONEAREST), &info)) return info.rcWork;
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0))
        work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    return work;
}
SIZE MinimumWindowSize(UINT dpi, const RECT& work) {
    RECT rect{0, 0, MulDiv(320, static_cast<int>(dpi), 96), MulDiv(280, static_cast<int>(dpi), 96)};
    // AdjustWindowRectExForDpi does not reserve space for standard scrollbars.
    // The form needs a genuine 320-DIP client viewport after its vertical bar appears.
    rect.right += GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
    rect.bottom += GetSystemMetricsForDpi(SM_CYHSCROLL, dpi);
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL, TRUE, 0, dpi);
    return {(std::min)(rect.right - rect.left, work.right - work.left),
            (std::min)(rect.bottom - rect.top, work.bottom - work.top)};
}
namespace {
RECT MeasureText(HWND control, int width, UINT format) {
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    text.resize(GetWindowTextW(control, text.data(), static_cast<int>(text.size())));
    // Empty controls still need one baseline of vertical space.
    if (text.empty()) text = L" ";
    const auto dc = GetDC(control);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    const auto old = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    RECT bounds{0, 0, (std::max)(1, width), 0};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &bounds, format | DT_CALCRECT);
    SelectObject(dc, old); ReleaseDC(control, dc);
    return bounds;
}
}
SIZE ControlTextSize(HWND control) {
    const auto bounds = MeasureText(control, 0, DT_SINGLELINE);
    return {bounds.right, bounds.bottom};
}
int WrappedTextHeight(HWND control, int width) {
    return MeasureText(control, width, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL).bottom;
}
void ClipChildToViewport(HWND child, HWND parent, const RECT& viewport) {
    RECT bounds{}; GetWindowRect(child, &bounds);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&bounds), 2);
    RECT visible{};
    if (IntersectRect(&visible, &bounds, &viewport)) OffsetRect(&visible, -bounds.left, -bounds.top);
    const auto region = CreateRectRgnIndirect(&visible);
    if (region && !SetWindowRgn(child, region, FALSE)) DeleteObject(region);
}
}
