#pragma once
#include <windows.h>

namespace ui {
// Screen coordinates throughout; normal window geometry is stored separately from
// minimized/maximized geometry so a disconnected monitor cannot hide the window.
RECT FitToWorkArea(RECT requested, const RECT& work);
RECT WorkAreaFor(RECT requested);
SIZE MinimumWindowSize(UINT dpi, const RECT& work);
// Measure the actual control font, including wrapping and explicit line breaks.
SIZE ControlTextSize(HWND control);
int WrappedTextHeight(HWND control, int width);
// Child coordinates remain unchanged; only the part inside viewport can paint or receive clicks.
void ClipChildToViewport(HWND child, HWND parent, const RECT& viewport);
}
