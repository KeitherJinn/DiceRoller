// Exercise our own hidden Win32 windows in-process. No input is injected into the desktop.
#include "../main.cpp"
#include <chrono>
#include <fstream>

namespace {
void CheckUi(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct TestWindow {
    App app;
    ATOM type = 0;
    explicit TestWindow(const std::filesystem::path& base, bool restore = false) {
        app.base = base;
        app.settings.csvPath = (base / L"dice_results_v2.csv").wstring();
        app.settingsPath = base / L"settings.bin";
        if (restore) {
            const auto loaded = dice::LoadSettings(app.settingsPath, app.settings);
            CheckUi(loaded.found && loaded.warning.empty(), "Could not reload the test settings.");
            app.settings = loaded.settings;
        }
        // Unexpected confirmations and picker calls must never open desktop UI.
        app.confirm = [](HWND, LPCWSTR, LPCWSTR, UINT) { return IDNO; };
        app.filePicker = [](OPENFILENAMEW*) { return FALSE; };
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_PROGRESS_CLASS};
        CheckUi(InitCommonControlsEx(&init), "Common controls could not be initialized.");
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DiceRollerRegressionWindow";
        wc.lpszMenuName = MAKEINTRESOURCEW(IDC_DICEROLLER);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_DICEROLLER));
        type = RegisterClassExW(&wc);
        CheckUi(type != 0, "Could not register test window.");
        CheckUi(CreateWindowExW(0, wc.lpszClassName, L"Dice Roller integration test", MainStyle,
            0, 0, 900, 760, nullptr, nullptr, wc.hInstance, &app) != nullptr, "Could not create test window.");
    }
    ~TestWindow() {
        app.job.Cancel();
        if (IsWindow(app.window)) DestroyWindow(app.window);
        UnregisterClassW(MAKEINTATOM(type), GetModuleHandleW(nullptr));
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
    }
};
RECT Bounds(HWND child, HWND parent) {
    RECT bounds{};
    GetWindowRect(child, &bounds);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&bounds), 2);
    return bounds;
}
void ResizeClient(App& app, int width, int height, UINT dpi) {
    RECT window{0, 0, MulDiv(width, static_cast<int>(dpi), 96), MulDiv(height, static_cast<int>(dpi), 96)};
    AdjustWindowRectExForDpi(&window, MainStyle, TRUE, 0, GetDpiForWindow(app.window));
    window.right -= window.left;
    window.bottom -= window.top;
    window.left = window.top = 0;
    SendMessageW(app.window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&window));
}
void WaitUi(App& app) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (Busy(app) && std::chrono::steady_clock::now() < deadline) {
        Poll(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CheckUi(!Busy(app), "UI job did not complete.");
}
bool Contains(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.top >= outer.top &&
        inner.right <= outer.right && inner.bottom <= outer.bottom;
}
void SetRoll(App& app, const wchar_t* notation = L"1d1+0", const wchar_t* times = L"2") {
    SetWindowTextW(Control(app, IDC_DICE_EDIT), notation);
    SetWindowTextW(Control(app, IDC_TIMES_EDIT), times);
    CheckRadioButton(app.window, IDC_RAD_NORMAL, IDC_RAD_DISADV, IDC_RAD_NORMAL);
    SendMessageW(Control(app, IDC_AUTOSAVE), BM_SETCHECK, BST_UNCHECKED, 0);
}
void CheckVirtualLayout(App& app) {
    for (HWND child = GetWindow(app.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        auto bounds = Bounds(child, app.window);
        OffsetRect(&bounds, app.scrollX, app.scrollY);
        const RECT content{0, 0, app.contentWidth + 1, app.contentHeight + 1};
        CheckUi(Contains(content, bounds), "A control falls outside the scrollable content.");
    }
    const int actions[] = {IDC_ROLL, IDC_CANCEL, IDC_SAVE, IDC_DISCARD, IDC_OPEN, IDC_COPY, IDC_CLEAR};
    for (std::size_t i = 0; i < std::size(actions); ++i)
        for (std::size_t j = i + 1; j < std::size(actions); ++j) {
            const auto first = Bounds(Control(app, actions[i]), app.window);
            const auto second = Bounds(Control(app, actions[j]), app.window);
            RECT overlap{};
            CheckUi(!IntersectRect(&overlap, &first, &second), "Action buttons overlap.");
        }
}
}

void UiRegressionTests(const std::filesystem::path& base) {
    TestWindow test(base);
    auto& app = test.app;
    CheckUi(GetMenu(app.window) != nullptr, "Menu resource is not attached.");
    CheckUi(GetClassLongPtrW(app.window, GCLP_HICON) != 0, "Icon resource is not attached.");
    CheckUi(FindResourceW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUTBOX), RT_DIALOG) != nullptr,
        "About dialog resource is missing.");
    CheckUi(!Checked(app, IDC_TIMESTAMP) && Checked(app, IDC_AUTOSAVE), "Unexpected default options.");

    for (UINT dpi : {96u, 144u, 192u}) {
        for (int width : {560, 780, 1000}) {
            ResizeClient(app, width, 550, dpi);
            CheckVirtualLayout(app);
        }
        LOGFONTW font{};
        GetObjectW(app.font, sizeof(font), &font);
        CheckUi(font.lfHeight == -MulDiv(10, static_cast<int>(dpi), 72), "Font was not rescaled.");
    }
    ResizeClient(app, 840, 680, 96);
    const int tabOrder[] = {IDC_DICE_EDIT, IDC_TIMES_EDIT, IDC_RAD_NORMAL};
    for (int id : tabOrder) CheckUi(GetWindowLongPtrW(Control(app, id), GWL_STYLE) & WS_TABSTOP, "Missing tab stop.");
    SetFocus(Control(app, IDC_DICE_EDIT));
    for (std::size_t i = 1; i < std::size(tabOrder); ++i) {
        MSG key{};
        key.hwnd = GetFocus(); key.message = WM_KEYDOWN; key.wParam = VK_TAB;
        CheckUi(IsDialogMessageW(app.window, &key), "Tab was not consumed.");
        CheckUi(GetFocus() == Control(app, tabOrder[i]), "Tab order is incorrect.");
    }
    MSG arrow{};
    arrow.hwnd = GetFocus(); arrow.message = WM_KEYDOWN; arrow.wParam = VK_RIGHT;
    IsDialogMessageW(app.window, &arrow);
    CheckUi(Checked(app, IDC_RAD_ADV), "Arrow key did not select the next radio button.");
    SetFocus(Control(app, IDC_FILE_EDIT));
    MSG fileTab{};
    fileTab.hwnd = GetFocus(); fileTab.message = WM_KEYDOWN; fileTab.wParam = VK_TAB;
    CheckUi(IsDialogMessageW(app.window, &fileTab) && GetFocus() == Control(app, IDC_BROWSE),
        "CSV path does not tab to Browse.");

    CheckRadioButton(app.window, IDC_RAD_NORMAL, IDC_RAD_DISADV, IDC_RAD_NORMAL);
    SetWindowTextW(Control(app, IDC_DICE_EDIT), L"1d1+0");
    SetWindowTextW(Control(app, IDC_TIMES_EDIT), L"2");
    SendMessageW(Control(app, IDC_AUTOSAVE), BM_SETCHECK, BST_UNCHECKED, 0);
    SetFocus(Control(app, IDC_DICE_EDIT));
    MSG enter{};
    enter.hwnd = GetFocus(); enter.message = WM_KEYDOWN; enter.wParam = VK_RETURN;
    IsDialogMessageW(app.window, &enter);
    CheckUi(Busy(app) && !IsWindowEnabled(Control(app, IDC_ROLL)), "Enter did not start a background operation.");
    CheckUi(!IsWindowEnabled(Control(app, IDC_PRESET_SAVE)) && !IsWindowEnabled(Control(app, IDC_QUICK_FIRST)),
        "Preset and quick-die controls remain enabled while rolling.");
    WaitUi(app);
    CheckUi(app.pending && app.pending->results.size() == 2, "Unsaved results were not retained.");
    CheckUi(IsWindowEnabled(Control(app, IDC_SAVE)) && !IsWindowEnabled(Control(app, IDC_ROLL)), "Pending-batch controls incorrect.");
    CheckUi(Text(Control(app, IDC_OUTPUT)).find(L"(1) = 1 -> Final: 1") != std::wstring::npos, "Formatted output missing.");
    app.log.Clear(); RefreshLog(app);
    CheckUi(app.pending->results.size() == 2, "Clearing the display discarded unsaved results.");
    SavePending(app); WaitUi(app);
    CheckUi(!app.pending && IsWindowEnabled(Control(app, IDC_ROLL)), "Save did not unlock new batches.");
    CheckUi(std::filesystem::exists(base / L"dice_results_v2.csv"), "UI save did not create CSV.");

    // Stress display retention without ever sending key/mouse input to another process.
    for (int i = 0; i < 1000; ++i) app.log.Add(std::wstring(300, L'x') + L"\r\n");
    RefreshLog(app);
    CheckUi(!app.logFailed && Text(Control(app, IDC_OUTPUT)).size() <= dice::LogBuffer::MaxCharacters,
        "Large log failed to display within its budget.");
}

void UiFeatureTests(const std::filesystem::path& base) {
    const auto selected = (base / L"chosen.csv").wstring();
    {
        TestWindow test(base);
        auto& app = test.app;
        for (const auto* input : {L"", L"wrong.txt"}) {
            SetWindowTextW(Control(app, IDC_FILE_EDIT), input);
            bool called = false;
            app.filePicker = [&](OPENFILENAMEW* picker) {
                called = true;
                CheckUi(std::wstring(picker->lpstrFile) == L"dice_results_v2.csv", "Invalid path did not fall back to a CSV filename.");
                CheckUi(picker->lpstrInitialDir && std::filesystem::is_directory(picker->lpstrInitialDir),
                    "File picker did not receive an existing initial directory.");
                CheckUi((picker->Flags & OFN_NOCHANGEDIR) != 0, "File picker can change the working directory.");
                return FALSE;
            };
            Browse(app);
            CheckUi(called && Text(Control(app, IDC_FILE_EDIT)) == input, "Cancelled browsing changed the original input.");
        }
        const auto missing = PrepareBrowse(L"new/subfolder/session.csv", base);
        CheckUi(std::filesystem::is_directory(missing.directory) && missing.filename == L"session.csv",
            "Browsing a missing parent did not use an existing ancestor.");
        app.filePicker = [&](OPENFILENAMEW* picker) {
            CheckUi(selected.size() + 1 < picker->nMaxFile, "File-picker test buffer is too small.");
            std::copy(selected.begin(), selected.end(), picker->lpstrFile);
            picker->lpstrFile[selected.size()] = L'\0';
            return TRUE;
        };
        Browse(app);
        CheckUi(Text(Control(app, IDC_FILE_EDIT)) == selected, "Accepted CSV selection was not applied.");

        SetWindowTextW(Control(app, IDC_TIMES_EDIT), L"7");
        SetWindowTextW(Control(app, IDC_NAME_EDIT), L"Original name");
        CheckRadioButton(app.window, IDC_RAD_NORMAL, IDC_RAD_DISADV, IDC_RAD_ADV);
        for (int i = 0; i < 7; ++i) {
            QuickDie(app, i);
            CheckUi(Text(Control(app, IDC_DICE_EDIT)) == L"1d" + std::to_wstring(QuickSides[i]), "A quick die has the wrong formula.");
            CheckUi(Text(Control(app, IDC_TIMES_EDIT)) == L"7" && Checked(app, IDC_RAD_ADV) &&
                Text(Control(app, IDC_NAME_EDIT)) == L"Original name", "Quick dice changed unrelated roll settings.");
            CheckUi(!Busy(app), "Selecting a quick die unexpectedly rolled it.");
        }
        SetWindowTextW(Control(app, IDC_PRESET), L"Attack");
        SetWindowTextW(Control(app, IDC_DICE_EDIT), L"2d6+3");
        SavePreset(app);
        CheckUi(app.settings.presets.size() == 1 && std::filesystem::exists(app.settingsPath), "New preset was not saved.");
        SetRoll(app, L"1d4", L"1");
        ApplyPreset(app);
        CheckUi(Text(Control(app, IDC_DICE_EDIT)) == L"2d6+3" && Text(Control(app, IDC_TIMES_EDIT)) == L"7" &&
            Checked(app, IDC_RAD_ADV) && Text(Control(app, IDC_NAME_EDIT)) == L"Attack", "Preset application did not restore its parameters.");

        SetWindowTextW(Control(app, IDC_PRESET), L"attack");
        SetWindowTextW(Control(app, IDC_DICE_EDIT), L"1d20+5");
        SetWindowTextW(Control(app, IDC_TIMES_EDIT), L"3");
        int confirmations = 0;
        app.confirm = [&](HWND, LPCWSTR, LPCWSTR, UINT) { ++confirmations; return IDNO; };
        SavePreset(app);
        CheckUi(confirmations == 1 && app.settings.presets.size() == 1 && app.settings.presets[0].notation == L"2d6+3",
            "Declining replacement altered a preset or case-insensitive matching failed.");
        app.confirm = [&](HWND, LPCWSTR, LPCWSTR, UINT) { ++confirmations; return IDYES; };
        SavePreset(app);
        CheckUi(confirmations == 2 && app.settings.presets.size() == 1 && app.settings.presets[0].notation == L"1d20+5",
            "Confirmed replacement duplicated or failed to update the preset.");

        SetWindowTextW(Control(app, IDC_NAME_EDIT), L"Remembered name");
        SendMessageW(Control(app, IDC_TIMESTAMP), BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(Control(app, IDC_AUTOSAVE), BM_SETCHECK, BST_UNCHECKED, 0);
        Close(app);
        CheckUi(!IsWindow(app.window), "Closing a configured idle window failed.");
    }
    {
        TestWindow reopened(base, true);
        auto& app = reopened.app;
        CheckUi(Text(Control(app, IDC_DICE_EDIT)) == L"1d20+5" && Text(Control(app, IDC_TIMES_EDIT)) == L"3" &&
            Text(Control(app, IDC_NAME_EDIT)) == L"Remembered name" && Checked(app, IDC_RAD_ADV) &&
            Checked(app, IDC_TIMESTAMP) && !Checked(app, IDC_AUTOSAVE) && Text(Control(app, IDC_FILE_EDIT)) == selected,
            "Roll preferences were not restored after reopening.");
        CheckUi(app.settings.presets.size() == 1 && SendMessageW(Control(app, IDC_PRESET), CB_GETCOUNT, 0, 0) == 1,
            "Saved presets were not restored to the selector.");
        SetWindowTextW(Control(app, IDC_PRESET), L"ATTACK");
        app.confirm = [](HWND, LPCWSTR, LPCWSTR, UINT) { return IDNO; };
        DeletePreset(app);
        CheckUi(app.settings.presets.size() == 1, "Declining deletion removed the preset.");
        app.confirm = [](HWND, LPCWSTR, LPCWSTR, UINT) { return IDYES; };
        DeletePreset(app);
        const auto stored = dice::LoadSettings(app.settingsPath, app.settings);
        CheckUi(app.settings.presets.empty() && stored.warning.empty() && stored.settings.presets.empty(),
            "Confirmed deletion did not persist.");
        app.settings.presets.push_back({L"  中文预设😀  ", L"2d8-1", 2, dice::Mode::Disadvantage});
        RefreshPresets(app, L"中文预设😀");
        ApplyPreset(app);
        CheckUi(Text(Control(app, IDC_DICE_EDIT)) == L"2d8-1" && Checked(app, IDC_RAD_DISADV),
            "A valid loaded Unicode preset with surrounding whitespace could not be applied.");
    }
    {
        TestWindow test(base);
        auto& app = test.app;
        app.settingsPath = base / L"future-settings.bin";
        { std::ofstream file(app.settingsPath, std::ios::binary); file << "future configuration"; }
        app.settingsNeedBackup = true;
        Close(app);
        CheckUi(!IsWindow(app.window), "Default-settings recovery could not close.");
        bool backupFound = false;
        for (const auto& entry : std::filesystem::directory_iterator(base)) {
            if (entry.path().extension() != L".bak") continue;
            std::ifstream file(entry.path(), std::ios::binary);
            const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            backupFound = backupFound || bytes == "future configuration";
        }
        CheckUi(backupFound && dice::LoadSettings(app.settingsPath, app.settings).warning.empty(),
            "Saving defaults did not preserve the unsupported original configuration.");
    }
}

void UiCloseTests(const std::filesystem::path& base) {
    for (int response : {IDYES, IDNO}) {
        TestWindow test(base);
        auto& app = test.app;
        SetRoll(app);
        StartRoll(app);
        int confirmations = 0;
        app.confirm = [&](HWND, LPCWSTR, LPCWSTR, UINT) {
            ++confirmations;
            // Simulate the modal dialog pumping timers until the worker is collected.
            WaitUi(app);
            CheckUi(app.pending && !Busy(app), "Close race was not reproduced.");
            Close(app);
            CheckUi(confirmations == 1, "A nested close showed another confirmation.");
            return response;
        };
        Close(app);
        CheckUi(confirmations == 1, "Busy close did not ask once.");
        if (response == IDYES)
            CheckUi(!IsWindow(app.window), "A task completed during close confirmation left its window alive.");
        else
            CheckUi(IsWindow(app.window) && app.pending && !app.closing && !app.closePrompt &&
                IsWindowEnabled(Control(app, IDC_SAVE)), "Declining close lost the finished unsaved batch.");
    }
    {
        TestWindow test(base);
        auto& app = test.app;
        SetRoll(app, L"1000d6", L"1000");
        StartRoll(app);
        app.confirm = [&](HWND, LPCWSTR, LPCWSTR, UINT) {
            CheckUi(Busy(app), "The busy-close branch was not reached.");
            return IDYES;
        };
        Close(app);
        CheckUi(app.closing && IsWindow(app.window) && !IsWindowEnabled(Control(app, IDC_CANCEL)),
            "Busy close did not wait for cancellation.");
        WaitUi(app);
        CheckUi(!IsWindow(app.window), "Cancelled worker did not finish closing the window.");
    }
    {
        TestWindow test(base);
        auto& app = test.app;
        SetRoll(app);
        StartRoll(app); WaitUi(app);
        const auto retained = app.pending;
        app.settingsPath = base / L"directory-instead-of-settings";
        std::filesystem::create_directory(app.settingsPath);
        int confirmations = 0;
        app.confirm = [&](HWND, LPCWSTR, LPCWSTR, UINT) { return ++confirmations == 1 ? IDYES : IDNO; };
        Close(app);
        CheckUi(confirmations == 2 && IsWindow(app.window) && !app.closing && app.pending == retained,
            "Declining exit after settings failure lost the batch or trapped the close state.");
        app.settingsPath = base / L"recovered-settings.bin";
        app.confirm = [](HWND, LPCWSTR, LPCWSTR, UINT) { return IDYES; };
        Close(app);
        CheckUi(!IsWindow(app.window), "A settings failure prevented a later successful close.");
    }
}

void UiViewportTests(const std::filesystem::path& base) {
    const RECT screen{0, 0, 1920, 1040};
    const RECT huge{2500, 1500, 4300, 3100};
    const auto fitted = ui::FitToWorkArea(huge, screen);
    CheckUi(Contains(screen, fitted) && fitted.right - fitted.left == 1800 && fitted.bottom - fitted.top == 1040,
        "Oversized off-screen windows do not fit a 1080p work area.");
    const RECT negative{-1920, -200, 0, 840};
    const auto moved = ui::FitToWorkArea(RECT{-4000, -900, -3000, -200}, negative);
    CheckUi(Contains(negative, moved) && moved.right - moved.left == 1000 && moved.bottom - moved.top == 700,
        "Window recovery does not support negative monitor coordinates.");
    const auto disconnected = ui::FitToWorkArea(RECT{-3000, 20, -2100, 720}, screen);
    CheckUi(Contains(screen, disconnected), "A disconnected monitor leaves the restored window off-screen.");
    for (UINT dpi : {96u, 144u, 192u, 288u}) {
        const RECT tiny{0, 0, 800, 500};
        const auto size = ui::MinimumWindowSize(dpi, tiny);
        CheckUi(size.cx > 0 && size.cy > 0 && size.cx <= 800 && size.cy <= 500,
            "Minimum tracking size exceeds the available work area.");
    }

    TestWindow test(base);
    auto& app = test.app;
    for (UINT dpi : {96u, 144u, 192u}) {
        ResizeClient(app, 1000, 900, dpi);
        RECT actual{};
        GetWindowRect(app.window, &actual);
        CheckUi(Contains(ui::WorkAreaFor(actual), actual), "DPI change placed the actual window outside the work area.");
        CheckVirtualLayout(app);
        MINMAXINFO limits{};
        SendMessageW(app.window, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limits));
        const auto work = ui::WorkAreaFor(actual);
        CheckUi(limits.ptMinTrackSize.x <= work.right - work.left && limits.ptMinTrackSize.y <= work.bottom - work.top,
            "Window minimum size is larger than its monitor.");
    }
    ResizeClient(app, 560, 300, 192);
    RECT viewport{};
    GetClientRect(app.window, &viewport);
    CheckUi(app.contentHeight > viewport.bottom, "Small high-DPI window does not expose scrollable content.");
    Scroll(app, SB_VERT, SB_BOTTOM);
    const auto footer = Bounds(Control(app, IDC_RETENTION), app.window);
    CheckUi(app.scrollY > 0 && footer.bottom <= viewport.bottom && footer.top >= 0,
        "Scrolling to the bottom does not reveal the final content.");
    SetFocus(Control(app, IDC_PRESET_SAVE));
    EnsureFocusVisible(app);
    CheckUi(Contains(viewport, Bounds(Control(app, IDC_PRESET_SAVE), app.window)),
        "Keyboard focus did not scroll the first-row button into view.");
    SetFocus(Control(app, IDC_BROWSE));
    EnsureFocusVisible(app);
    CheckUi(Contains(viewport, Bounds(Control(app, IDC_BROWSE), app.window)),
        "Keyboard focus did not scroll Browse into view.");
    Scroll(app, SB_VERT, SB_TOP);
    SendMessageW(app.window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
    CheckUi(app.scrollY > 0, "Mouse wheel does not scroll the form.");

    // Deliberately undersize our hidden test window to exercise horizontal fallback.
    SetWindowPos(app.window, nullptr, 0, 0, 420, 500, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // Windows enforces the physical minimum during SetWindowPos. Simulate a
    // still higher scale in that fixed viewport, as on a narrow high-DPI monitor.
    app.dpi = 384;
    SetFont(app);
    Layout(app);
    GetClientRect(app.window, &viewport);
    CheckUi(app.contentWidth > viewport.right, "Narrow viewport has no horizontal fallback.");
    SetFocus(Control(app, IDC_PRESET_DELETE));
    EnsureFocusVisible(app);
    const auto deleteBounds = Bounds(Control(app, IDC_PRESET_DELETE), app.window);
    if (!(app.scrollX > 0 && Contains(viewport, deleteBounds)))
        throw std::runtime_error("Horizontal focus: scroll=" + std::to_string(app.scrollX) +
            ", viewport=" + std::to_string(viewport.right) + "," + std::to_string(viewport.bottom) +
            ", button=" + std::to_string(deleteBounds.left) + "," + std::to_string(deleteBounds.top) +
            "," + std::to_string(deleteBounds.right) + "," + std::to_string(deleteBounds.bottom));

    ResizeClient(app, 840, 680, 96);
    const auto oldLog = Bounds(Control(app, IDC_OUTPUT), app.window);
    const int oldWidth = app.contentWidth;
    ResizeClient(app, 1000, 900, 96);
    const auto newLog = Bounds(Control(app, IDC_OUTPUT), app.window);
    if (app.contentWidth > oldWidth)
        CheckUi(newLog.right - newLog.left > oldLog.right - oldLog.left, "Log does not grow with available width.");
    CheckUi(newLog.bottom - newLog.top >= oldLog.bottom - oldLog.top, "Log shrinks when more space is available.");
    RememberWindow(app);
    RECT normal{};
    GetWindowRect(app.window, &normal);
    CheckUi(app.settings.window.valid && app.settings.window.x == normal.left && app.settings.window.y == normal.top &&
        app.settings.window.width == MulDiv(normal.right - normal.left, 96, static_cast<int>(app.dpi)),
        "Normal window geometry is not remembered in screen coordinates and DIP size.");
    const auto remembered = app.settings.window;
    // Exercise bookkeeping without showing or focusing the hidden test window.
    const auto style = GetWindowLongPtrW(app.window, GWL_STYLE);
    SetWindowLongPtrW(app.window, GWL_STYLE, style | WS_MAXIMIZE);
    RememberWindow(app);
    CheckUi(app.settings.window.maximized && app.settings.window.x == remembered.x &&
        app.settings.window.width == remembered.width, "Maximizing replaced the remembered normal geometry.");
    SetWindowLongPtrW(app.window, GWL_STYLE, (style & ~WS_MAXIMIZE) | WS_MINIMIZE);
    RememberWindow(app);
    CheckUi(app.settings.window.maximized && app.settings.window.y == remembered.y &&
        app.settings.window.height == remembered.height, "Minimizing replaced the remembered restore state.");
    SetWindowLongPtrW(app.window, GWL_STYLE, style);
}
