#include "WindowLayout.h"
#include <algorithm>

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
    RECT rect{0, 0, MulDiv(560, static_cast<int>(dpi), 96), MulDiv(300, static_cast<int>(dpi), 96)};
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL, TRUE, 0, dpi);
    return {(std::min)(rect.right - rect.left, work.right - work.left),
            (std::min)(rect.bottom - rect.top, work.bottom - work.top)};
}
}
