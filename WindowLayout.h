#pragma once
#include <windows.h>

namespace ui {
// Screen coordinates throughout; normal window geometry is stored separately from
// minimized/maximized geometry so a disconnected monitor cannot hide the window.
RECT FitToWorkArea(RECT requested, const RECT& work);
RECT WorkAreaFor(RECT requested);
SIZE MinimumWindowSize(UINT dpi, const RECT& work);
}
