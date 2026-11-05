#pragma once
// Included in UiTests.cpp to share the hidden-window fixture without desktop input.
namespace {
void CheckMainBodyClip(App& app) {
    RECT client{}; GetClientRect(app.window, &client);
    const RECT body{0, 0, client.right, app.bodyHeight};
    const auto bodyBounds = Bounds(app.body, app.window);
    CheckUi(GetParent(app.body) == app.window && EqualRect(&bodyBounds, &body),
        "The native body viewport does not stop at the fixed footer boundary.");
    CheckUi((GetWindowLongPtrW(app.window, GWL_STYLE) & WS_CLIPCHILDREN) != 0 &&
        (GetWindowLongPtrW(app.body, GWL_EXSTYLE) & WS_EX_CONTROLPARENT) != 0,
        "The form viewport does not isolate painting while preserving keyboard navigation.");
    for (HWND child = GetWindow(app.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (child == app.body) continue;
        const auto bounds = Bounds(child, app.window);
        if (IsFooterControl(GetDlgCtrlID(child))) {
            if (!Contains(client, bounds) || bounds.top < app.bodyHeight)
                throw std::runtime_error("Footer outside viewport: id=" + std::to_string(GetDlgCtrlID(child)) +
                    " dpi=" + std::to_string(app.dpi) + " client=" + std::to_string(client.right) + "," + std::to_string(client.bottom) +
                    " bounds=" + std::to_string(bounds.left) + "," + std::to_string(bounds.top) + "," + std::to_string(bounds.right) + "," + std::to_string(bounds.bottom));
        }
        else CheckUi(false, "A scrolling control is parented directly to the footer window.");
    }
    for (HWND child = GetWindow(app.body, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        CheckUi(!IsFooterControl(GetDlgCtrlID(child)), "A fixed action was placed inside the scrolling viewport.");
        const auto bounds = Bounds(child, app.window);
        auto content = bounds; OffsetRect(&content, app.scrollX, app.scrollY);
        const RECT virtualBody{0, 0, app.contentWidth, app.contentHeight};
        CheckUi(Contains(virtualBody, content), "Responsive body layout placed a control outside its content extent.");
        const auto region = CreateRectRgn(0, 0, 0, 0);
        CheckUi(region != nullptr, "Could not allocate clipping-test region.");
        const auto kind = GetWindowRgn(child, region); DeleteObject(region);
        CheckUi(kind == ERROR, "A scrolling control still depends on its own mutable window region for clipping.");
    }
}
void CheckMeasuredMainText(App& app) {
    for (int id : {IDC_PRESET_LABEL, IDC_GROUP_LABEL, IDC_QUICK_LABEL, IDC_DICE_LABEL,
        IDC_TIMES_LABEL, IDC_NAME_LABEL, IDC_FILE_LABEL}) {
        const auto bounds = Bounds(Control(app, id), app.window);
        const auto text = ui::ControlTextSize(Control(app, id));
        CheckUi(bounds.right - bounds.left >= text.cx && bounds.bottom - bounds.top >= text.cy,
            "A form label is clipped because its width was derived from a different caption.");
    }
    const auto quick = Bounds(Control(app, IDC_QUICK_IMMEDIATE), app.window);
    const auto quickText = ui::ControlTextSize(Control(app, IDC_QUICK_IMMEDIATE));
    CheckUi(quick.right - quick.left >= quickText.cx + Px(app, 26) && quick.bottom - quick.top >= quickText.cy + Px(app, 4),
        "The immediate quick-roll checkbox clips its label at high DPI.");
    for (int id = IDC_QUICK_FIRST; id <= IDC_QUICK_LAST; ++id)
        CheckUi(Bounds(Control(app, id), app.window).bottom <= quick.top, "The quick-roll switch overlaps a dice button.");
    CheckUi(quick.bottom <= Bounds(Control(app, IDC_DICE_EDIT), app.window).top, "The quick-roll switch overlaps the dice expression.");
    for (int id : {IDC_RULES, IDC_RETENTION, IDC_OUTPUT_LABEL}) {
        const auto bounds = Bounds(Control(app, id), app.window);
        CheckUi(bounds.bottom - bounds.top >= ui::WrappedTextHeight(Control(app, id), bounds.right - bounds.left),
            "A main-window explanation is clipped instead of wrapping to its actual font height.");
    }
    for (int id : {IDC_PRESET_APPLY, IDC_PRESET_SAVE, IDC_PRESET_DELETE, IDC_IMPORT, IDC_EXPORT_PRESETS,
        IDC_ROLL, IDC_CANCEL, IDC_SAVE, IDC_DISCARD, IDC_PROBABILITY, IDC_OPEN, IDC_COPY, IDC_CLEAR, IDC_BROWSE}) {
        const auto bounds = Bounds(Control(app, id), app.window);
        const auto text = ui::ControlTextSize(Control(app, id));
        CheckUi(bounds.right - bounds.left >= text.cx + Px(app, 8) && bounds.bottom - bounds.top >= text.cy + Px(app, 4),
            "A responsive action button clips its label.");
    }
}
std::vector<LONG> MainLayoutSnapshot(App& app) {
    std::vector<LONG> values{app.contentWidth, app.contentHeight, app.bodyHeight, app.scrollX, app.scrollY};
    RECT client{}; GetClientRect(app.window, &client);
    values.insert(values.end(), {client.right, client.bottom});
    auto append = [&](HWND child) {
        const auto bounds = Bounds(child, app.window);
        values.insert(values.end(), {GetDlgCtrlID(child), bounds.left, bounds.top, bounds.right, bounds.bottom});
        const auto region = CreateRectRgn(0, 0, 0, 0);
        CheckUi(region != nullptr, "Could not allocate layout snapshot region.");
        const auto kind = GetWindowRgn(child, region); RECT clipped{};
        GetRgnBox(region, &clipped); DeleteObject(region);
        values.insert(values.end(), {kind, clipped.left, clipped.top, clipped.right, clipped.bottom});
    };
    for (HWND parent : {app.window, app.body})
        for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) append(child);
    for (int bar : {SB_HORZ, SB_VERT}) {
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        GetScrollInfo(app.window, bar, &info);
        values.insert(values.end(), {info.nMin, info.nMax, static_cast<LONG>(info.nPage), info.nPos});
    }
    return values;
}
void CheckMainLayoutSettled(App& app) {
    const auto before = MainLayoutSnapshot(app);
    Layout(app);
    CheckUi(before == MainLayoutSnapshot(app), "Resize left stale geometry or clipping until a second layout.");
}
struct MainRegionProbe {
    HWND window;
    int changes = 0;
    static LRESULT CALLBACK Observe(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
        if (message == WM_WINDOWPOSCHANGED) ++reinterpret_cast<MainRegionProbe*>(data)->changes;
        return DefSubclassProc(window, message, wParam, lParam);
    }
    explicit MainRegionProbe(HWND child) : window(child) {
        CheckUi(SetWindowSubclass(window, Observe, 0x4c5954, reinterpret_cast<DWORD_PTR>(this)) != FALSE,
            "Could not observe native clipping changes.");
    }
    ~MainRegionProbe() { RemoveWindowSubclass(window, Observe, 0x4c5954); }
};
void CheckMainResizeRegression(App& app) {
    ResizeClient(app, 800, 500, 96);
    SetFocus(Control(app, IDC_DICE_EDIT)); EnsureFocusVisible(app);
    Scroll(app, SB_VERT, SB_BOTTOM);
    const auto oldScroll = app.scrollY;
    CheckUi(oldScroll > 0 && Bounds(Control(app, IDC_DICE_EDIT), app.window).top < 0,
        "Resize regression did not leave the focused input outside the scrolled viewport.");
    RECT window{}; GetWindowRect(app.window, &window);
    for (int delta : {1, 2, 6, 0, 8, 0}) {
        SetWindowPos(app.window, nullptr, 0, 0, window.right - window.left + delta, window.bottom - window.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        CheckUi(app.scrollY == oldScroll && GetFocus() == Control(app, IDC_DICE_EDIT),
            "Dragging a window edge unexpectedly scrolled back to the previously focused input.");
        CheckMainBodyClip(app); CheckMainLayoutSettled(app);
    }
    for (UINT dpi : {144u, 192u, 288u, 96u}) {
        ResizeClient(app, 800, 500, dpi);
        for (int width : {340, 620, 800, 550, 720, 340}) {
            RECT requested{0, 0, Px(app, width), Px(app, 500)};
            AdjustWindowRectExForDpi(&requested, MainStyle, TRUE, 0, GetDpiForWindow(app.window));
            SetWindowPos(app.window, nullptr, 0, 0, requested.right - requested.left, requested.bottom - requested.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            CheckMainBodyClip(app); CheckMeasuredMainText(app); CheckMainLayoutSettled(app);
        }
    }
    ResizeClient(app, 800, 500, 96);
    SetFocus(Control(app, IDC_DICE_EDIT)); EnsureFocusVisible(app);
    RECT viewport{}; GetClientRect(app.body, &viewport);
    const auto dice = Control(app, IDC_DICE_EDIT);
    MainRegionProbe observer(dice);
    ui::ClipChildToViewport(dice, app.body, viewport);
    observer.changes = 0;
    ui::ClipChildToViewport(dice, app.body, viewport);
    CheckUi(observer.changes == 0, "An unchanged clip unnecessarily sends native position-change messages.");
    const auto diceBounds = Bounds(dice, app.window);
    auto partial = viewport; partial.right = diceBounds.right - 4;
    ui::ClipChildToViewport(dice, app.body, partial);
    CheckUi(observer.changes > 0, "A changed clip was not applied to the native control.");
    observer.changes = 0;
    ui::ClipChildToViewport(dice, app.body, partial);
    CheckUi(observer.changes == 0, "Repeating a partial clip unnecessarily resets the native region.");
    SetWindowRgn(dice, nullptr, FALSE);
    ui::RedrawLayout(app.window);
    CheckMainBodyClip(app);
}
}
void MainLayoutTests(const std::filesystem::path& base) {
    TestWindow test(base);
    auto& app = test.app;
    for (UINT dpi : {96u, 144u, 192u, 288u}) {
        for (int height : {280, 300, 600}) {
            ResizeClient(app, 320, height, dpi);
            Scroll(app, SB_VERT, SB_TOP);
            CheckUi(app.bodyHeight >= Px(app, 48), "The fixed footer consumed the complete form viewport.");
            CheckMainBodyClip(app); CheckMeasuredMainText(app);
            CheckUi(Bounds(Control(app, IDC_PRESET_APPLY), app.window).top >= Bounds(Control(app, IDC_PRESET), app.window).bottom,
                "Narrow preset controls did not reflow onto separate rows.");
            CheckUi(Bounds(Control(app, IDC_TIMES_EDIT), app.window).top >= Bounds(Control(app, IDC_DICE_EDIT), app.window).bottom,
                "Narrow dice and count inputs overlap instead of stacking.");
            std::vector<RECT> footer;
            for (int id : {IDC_ROLL, IDC_CANCEL, IDC_SAVE, IDC_DISCARD, IDC_PROGRESS, IDC_STATUS})
                footer.push_back(Bounds(Control(app, id), app.window));
            Scroll(app, SB_VERT, SB_BOTTOM);
            CheckMainBodyClip(app);
            std::size_t index = 0;
            for (int id : {IDC_ROLL, IDC_CANCEL, IDC_SAVE, IDC_DISCARD, IDC_PROGRESS, IDC_STATUS}) {
                const auto bounds = Bounds(Control(app, id), app.window);
                CheckUi(EqualRect(&bounds, &footer[index++]), "Scrolling moved a fixed action or progress/status control.");
            }
            const auto retention = Bounds(Control(app, IDC_RETENTION), app.window);
            CheckUi(retention.top >= 0 && retention.bottom <= app.bodyHeight,
                "The final retention explanation cannot be reached within the body viewport.");
            SetFocus(Control(app, IDC_BROWSE)); EnsureFocusVisible(app);
            RECT client{}; GetClientRect(app.window, &client); client.bottom = app.bodyHeight;
            CheckUi(Contains(client, Bounds(Control(app, IDC_BROWSE), app.window)),
                "Keyboard focus moved under the footer instead of scrolling into the body.");
            const auto oldScroll = app.scrollY;
            SetFocus(Control(app, IDC_ROLL)); EnsureFocusVisible(app);
            CheckUi(app.scrollY == oldScroll, "Focusing a fixed footer action unexpectedly moved the form.");
        }
    }
    ResizeClient(app, 320, 280, 96);
    const std::wstring longStatus(32760, L'W');
    Status(app, longStatus);
    CheckMainBodyClip(app);
    CheckUi(Text(Control(app, IDC_STATUS)) == longStatus && app.bodyHeight >= Px(app, 48),
        "A long error either lost text or made the actions unreachable.");
    CheckUi((GetWindowLongPtrW(Control(app, IDC_STATUS), GWL_STYLE) & (ES_MULTILINE | ES_READONLY | WS_VSCROLL)) ==
        (ES_MULTILINE | ES_READONLY | WS_VSCROLL), "Long status messages do not have a readable scrollable control.");
    Status(app, L"Ready.");
    ResizeClient(app, 1000, 900, 96); Scroll(app, SB_VERT, SB_TOP);
    CheckMainBodyClip(app); CheckMeasuredMainText(app);
    const auto first = Bounds(Control(app, IDC_ROLL), app.window);
    CheckUi(first.top == Bounds(Control(app, IDC_DISCARD), app.window).top,
        "A wide viewport did not restore the single-row action footer.");
    // Simulate a monitor narrower than the normal minimum, without changing the
    // desktop DPI. Horizontal fallback must expose every fixed footer action.
    ResizeClient(app, 320, 600, 96);
    app.dpi = 192; SetFont(app); app.scrollX = app.scrollY = 0; Layout(app);
    RECT small{}; GetClientRect(app.window, &small);
    CheckUi(app.contentWidth > small.right, "Undersized monitor fixture did not exercise horizontal fallback.");
    for (int id : {IDC_ROLL, IDC_CANCEL, IDC_SAVE, IDC_DISCARD}) {
        EnableWindow(Control(app, id), TRUE);
        const auto before = Bounds(Control(app, id), app.window);
        const auto oldScroll = app.scrollY;
        SetFocus(Control(app, id)); EnsureFocusVisible(app);
        const auto after = Bounds(Control(app, id), app.window);
        CheckUi(Contains(small, after) && after.top == before.top && app.scrollY == oldScroll,
            "An undersized monitor cannot horizontally reveal a vertically fixed action.");
        const auto region = CreateRectRgn(0, 0, 0, 0);
        const auto kind = GetWindowRgn(Control(app, id), region); RECT visible{};
        GetRgnBox(region, &visible); DeleteObject(region);
        CheckUi(kind != ERROR && visible.left == 0 && visible.right == after.right - after.left,
            "A focused footer action remains clipped after horizontal fallback.");
    }
    CheckMainResizeRegression(app);
}
