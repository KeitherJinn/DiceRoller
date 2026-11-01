#pragma once
// Included in UiTests.cpp to share the hidden-window fixture without desktop input.
namespace {
void CheckMainBodyClip(App& app) {
    RECT client{}; GetClientRect(app.window, &client);
    const RECT body{0, 0, client.right, app.bodyHeight};
    for (HWND child = GetWindow(app.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        const auto bounds = Bounds(child, app.window);
        if (IsFooterControl(GetDlgCtrlID(child))) {
            if (!Contains(client, bounds) || bounds.top < app.bodyHeight)
                throw std::runtime_error("Footer outside viewport: id=" + std::to_string(GetDlgCtrlID(child)) +
                    " dpi=" + std::to_string(app.dpi) + " client=" + std::to_string(client.right) + "," + std::to_string(client.bottom) +
                    " bounds=" + std::to_string(bounds.left) + "," + std::to_string(bounds.top) + "," + std::to_string(bounds.right) + "," + std::to_string(bounds.bottom));
            continue;
        }
        auto content = bounds; OffsetRect(&content, app.scrollX, app.scrollY);
        const RECT virtualBody{0, 0, app.contentWidth, app.contentHeight};
        CheckUi(Contains(virtualBody, content), "Responsive body layout placed a control outside its content extent.");
        RECT expected{};
        if (IntersectRect(&expected, &bounds, &body)) OffsetRect(&expected, -bounds.left, -bounds.top);
        const auto region = CreateRectRgn(0, 0, 0, 0);
        CheckUi(region != nullptr, "Could not allocate clipping-test region.");
        const auto kind = GetWindowRgn(child, region); RECT actual{}; GetRgnBox(region, &actual); DeleteObject(region);
        CheckUi(kind != ERROR && EqualRect(&actual, &expected), "A body control can paint or receive clicks over the fixed footer.");
    }
}
void CheckMeasuredMainText(App& app) {
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
}
