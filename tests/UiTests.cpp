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
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
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
    SetMode(app, dice::Mode::Normal);
    SendMessageW(Control(app, IDC_AUTOSAVE), BM_SETCHECK, BST_UNCHECKED, 0);
}
void CheckVirtualLayout(App& app) {
    CheckUi(IsWindow(app.body) && GetParent(app.body) == app.window, "Scrollable body must be a child viewport of the main window.");
    const auto bodyBounds = Bounds(app.body, app.window);
    CheckUi(bodyBounds.left == 0 && bodyBounds.top == 0 && bodyBounds.bottom == app.bodyHeight,
        "Body viewport must clip controls above the fixed footer.");
    for (HWND parent : {app.body, app.window}) {
        for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (child == app.body) continue;
            const bool footer = IsFooterControl(GetDlgCtrlID(child));
            CheckUi(footer == (parent == app.window), "Control belongs to the wrong body/footer container.");
            auto bounds = Bounds(child, app.window);
            RECT content{};
            if (footer) {
                GetClientRect(app.window, &content);
                content.top = app.bodyHeight;
            } else {
                OffsetRect(&bounds, app.scrollX, app.scrollY);
                content = {0, 0, app.contentWidth + 1, app.contentHeight + 1};
            }
            CheckUi(Contains(content, bounds), "A control falls outside its scrollable body or fixed footer.");
        }
    }
    const int actions[] = {IDC_ROLL, IDC_CANCEL, IDC_SAVE, IDC_DISCARD, IDC_OPEN, IDC_COPY, IDC_CLEAR};
    for (std::size_t i = 0; i < std::size(actions); ++i)
        for (std::size_t j = i + 1; j < std::size(actions); ++j) {
            auto first = Bounds(Control(app, actions[i]), app.window);
            auto second = Bounds(Control(app, actions[j]), app.window);
            RECT body{}; GetClientRect(app.body, &body);
            MapWindowPoints(app.body, app.window, reinterpret_cast<POINT*>(&body), 2);
            if (!IsFooterControl(actions[i])) IntersectRect(&first, &first, &body);
            if (!IsFooterControl(actions[j])) IntersectRect(&second, &second, &body);
            RECT overlap{};
            CheckUi(!IntersectRect(&overlap, &first, &second), "Action buttons overlap.");
        }
}
}

void UiAdvancedTests(const std::filesystem::path& base) {
    TestWindow test(base);
    auto& app = test.app;
    app.historyRoot = base / L"history";
    SetRoll(app, L"4d1kh3+1d1", L"3");
    SendMessageW(Control(app, IDC_TARGET), BM_SETCHECK, BST_CHECKED, 0);
    SetWindowTextW(Control(app, IDC_TARGET_VALUE), L"4");
    StartRoll(app); WaitUi(app);
    CheckUi(app.history.size() == 1 && HasPending(app) && !app.history[0].saved && !app.pending && app.dirtyHistory.empty(), "First durable batch missing or full data retained unnecessarily.");
    const auto firstId = app.history[0].id;
    CheckUi(Text(Control(app, IDC_OUTPUT)).find(L"Target MET") != std::wstring::npos, "Target feedback missing from log.");
    SetRoll(app, L"8d1", L"2");
    SendMessageW(Control(app, IDC_POOL), BM_SETCHECK, BST_CHECKED, 0);
    SetWindowTextW(Control(app, IDC_POOL_VALUE), L"1");
    StartRoll(app); WaitUi(app);
    CheckUi(app.history.size() == 2 && app.history[0].id == firstId && HasPending(app), "Continuous roll lost the previous pending batch.");
    auto restored = dice::LoadHistory(app.historyRoot);
    CheckUi(restored.entries.size() == 2 && restored.warnings.empty(), "Unsaved batches did not survive a history reload.");
    const auto first = dice::ReadHistoryEntry(app.historyRoot, firstId);
    CheckUi(dice::ReadHistoryEntry(app.historyRoot, app.history[1].id).batch.results.front().finalValue == 8, "Pool did not count per-die successes.");
    const auto detail = ui::FullRollDetail(first, 0);
    CheckUi(detail.find(L"dropped") != std::wstring::npos && detail.find(L"Target >= 4: MET") != std::wstring::npos,
        "Full history detail omitted keep/target audit information.");
    CheckUi(ui::BatchSummary(first).find(L"100.00%") != std::wstring::npos, "Batch target statistics are wrong.");
    RestoreRequest(app, app.history[0].request);
    const auto reused = ReadRequest(app);
    CheckUi(reused.notation == L"4d1kh3+1d1" && reused.target.enabled && reused.spec.successThreshold == 0 && !Busy(app),
        "Reuse changed old results, rolled implicitly or lost rule parameters.");
    SetWindowTextW(Control(app, IDC_FILE_EDIT), (base / L"all.csv").c_str());
    SavePending(app); WaitUi(app);
    CheckUi(!HasPending(app) && app.history[0].saved && app.history[1].saved, "Combined export did not mark every batch.");
    const auto bytes = std::filesystem::file_size(base / L"all.csv");
    app.job.StartExportIds(app.history, base / L"all.csv", app.historyRoot); app.saveOnly = true; WaitUi(app);
    CheckUi(std::filesystem::file_size(base / L"all.csv") == bytes, "Retry duplicated exported batches.");
    restored = dice::LoadHistory(app.historyRoot);
    CheckUi(restored.entries.size() == 2 && restored.entries[0].saved && restored.entries[1].saved, "Saved state did not survive restart.");
    for (UINT dpi : {96u,144u,192u,288u}) {
        ResizeClient(app, 560, 500, dpi);
        CheckVirtualLayout(app);
    }
    SetWindowTextW(Control(app, IDC_PRESET), L"Rules");
    SetWindowTextW(Control(app, IDC_GROUP), L"Character A");
    SavePreset(app);
    CheckUi(app.settings.presets.back().group == L"Character A" && app.settings.presets.back().target.enabled,
        "Preset omitted group or target settings.");
    // Durable pending results no longer require a discard confirmation on normal exit.
    SetRoll(app); SendMessageW(Control(app, IDC_TARGET), BM_SETCHECK, BST_UNCHECKED, 0);
    StartRoll(app); WaitUi(app);
    int confirmations = 0;
    app.confirm = [&](HWND, LPCWSTR, LPCWSTR, UINT) { ++confirmations; return IDNO; };
    Close(app);
    CheckUi(!IsWindow(app.window) && confirmations == 0 && dice::LoadHistory(app.historyRoot).entries.size() == 3,
        "Closing failed to preserve durable pending results without discarding.");
}

void UiInteractionTests(const std::filesystem::path& base) {
    {
        TestWindow test(base);
        auto& app = test.app;
        app.historyRoot = base / L"indexed";
        SetRoll(app, L"1000d6", L"10000");
        SetWindowTextW(Control(app, IDC_NAME_EDIT), L"");
        const auto probability = ReadRules(app);
        CheckUi(probability.times == 1 && probability.spec.count == 1000,
            "Single-roll analysis depends on the unrelated batch name or count.");
        bool rollRejected = false;
        try { ReadRequest(app); } catch (const std::exception&) { rollRejected = true; }
        CheckUi(rollRejected, "The actual roll must still validate name and batch limits.");
        SetRoll(app, L"2d6", L"3");
        SetWindowTextW(Control(app, IDC_PRESET), L"Named independently");
        SetWindowTextW(Control(app, IDC_GROUP), L"A");
        SavePreset(app);
        CheckUi(app.settings.presets.back().name == L"Named independently", "Saving a named preset incorrectly requires a roll name.");
        app.settings.presets.push_back({L"Second", L"d20", 1, dice::Mode::Normal, L"B"});
        SetWindowTextW(Control(app, IDC_GROUP), L"");
        RefreshPresets(app);
        const auto combo = Control(app, IDC_GROUP);
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        // Use the combo's own keyboard handling; selection notifications rebuild
        // only the dependent preset list, never the source group combo.
        SendMessageW(combo, WM_KEYDOWN, VK_DOWN, 0);
        CheckUi(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1, "First Down loses the group selection.");
        SendMessageW(combo, WM_KEYDOWN, VK_DOWN, 0);
        CheckUi(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 2 && Text(combo) == L"B", "Repeated Down gets stuck on the empty group.");
        CheckUi(SendMessageW(Control(app, IDC_PRESET), CB_GETCOUNT, 0, 0) == 1, "Group keyboard navigation does not filter presets.");
        SetWindowTextW(Control(app, IDC_TARGET_VALUE), L"15");
        SetWindowTextW(Control(app, IDC_POOL_VALUE), L"11");
        SendMessageW(Control(app, IDC_TARGET), BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(Control(app, IDC_POOL), BM_SETCHECK, BST_UNCHECKED, 0);
        CommitSettings(app, CaptureSettings(app));
        CheckUi(app.settings.target.value == 15 && app.settings.successThreshold == 11 && !app.settings.poolEnabled,
            "Disabled rule drafts were lost while another field was invalid.");
        SetWindowTextW(Control(app, IDC_NAME_EDIT), L"roll");
        StartRoll(app); WaitUi(app);
        CheckUi(app.history.size() == 1 && app.dirtyHistory.empty() && !app.pending,
            "Durable batches must retain metadata, not duplicate full dice data.");
        const auto id = app.history.front().id;
        app.history.clear();
        StartHistoryScan(app);
        CheckUi(Busy(app) && !IsWindowEnabled(Control(app, IDC_ROLL)), "Startup scanning permits concurrent rolls before the index is ready.");
        WaitUi(app);
        CheckUi(app.history.size() == 1 && app.history.front().id == id && HasPending(app), "Background metadata recovery lost the pending batch.");
        // An unchanged durable history file can be read-only at shutdown: there
        // is no reason to rewrite every pending result merely to close.
        const auto record = app.historyRoot / (dice::Wide(id) + L".dhist");
        CheckUi(SetFileAttributesW(record.c_str(), FILE_ATTRIBUTE_READONLY), "Could not protect the durable test record.");
        int prompts = 0;
        app.confirm = [&](HWND, LPCWSTR, LPCWSTR, UINT) { ++prompts; return IDNO; };
        Close(app);
        SetFileAttributesW(record.c_str(), FILE_ATTRIBUTE_NORMAL);
        CheckUi(!IsWindow(app.window) && prompts == 0, "Closing rewrote an unchanged durable record.");
    }
    {
        TestWindow restored(base, true);
        CheckUi(!Checked(restored.app, IDC_POOL) && Text(Control(restored.app, IDC_POOL_VALUE)) == L"11" &&
            !Checked(restored.app, IDC_TARGET) && Text(Control(restored.app, IDC_TARGET_VALUE)) == L"15",
            "Disabled target/pool values did not survive restart.");
    }
    {
        TestWindow test(base);
        auto& app = test.app;
        auto index = dice::ScanHistory(base / L"indexed");
        auto dirty = dice::ReadHistoryEntry(base / L"indexed", index.entries.front().id);
        dirty = dice::NewHistoryEntry(std::move(dirty.batch));
        const auto dirtyId = dirty.id;
        app.historyRoot = base / L"blocked-history";
        { std::ofstream blocker(app.historyRoot); blocker << "not a directory"; }
        app.dirtyHistory.push_back(dirty);
        auto summary = dice::SummaryOf(dirty); summary.localPersisted = false;
        app.history.push_back(summary);
        int prompts = 0;
        app.confirm = [&](HWND, LPCWSTR, LPCWSTR, UINT) { ++prompts; return IDNO; };
        Close(app);
        CheckUi(Busy(app) && app.flushingClose && IsWindow(app.window), "Recovery flush blocked the close handler.");
        WaitUi(app);
        CheckUi(IsWindow(app.window) && !app.closing && prompts == 1 && app.dirtyHistory.front().id == dirtyId,
            "Failed close flush discarded exact results or prevented staying open.");
        app.historyRoot = base / L"retry-history";
        Close(app); WaitUi(app);
        CheckUi(!IsWindow(app.window) && prompts == 1 && dice::ReadHistoryEntry(app.historyRoot, dirtyId).batch.results.size() == dirty.batch.results.size(),
            "Close retry failed to persist the retained batch exactly once.");
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
    SetFocus(Control(app, IDC_OUTPUT));
    MSG bodyTab{};
    bodyTab.hwnd = GetFocus(); bodyTab.message = WM_KEYDOWN; bodyTab.wParam = VK_TAB;
    const auto bodyTabConsumed = IsDialogMessageW(app.window, &bodyTab);
    if (!bodyTabConsumed || GetFocus() != Control(app, IDC_ROLL))
        throw std::runtime_error("Tab body to footer: consumed=" + std::to_string(bodyTabConsumed) +
            ", focus=" + std::to_string(GetDlgCtrlID(GetFocus())) + ", next=" +
            std::to_string(GetDlgCtrlID(GetNextDlgTabItem(app.window, Control(app, IDC_OUTPUT), FALSE))) +
            ", code=" + std::to_string(SendMessageW(Control(app, IDC_OUTPUT), WM_GETDLGCODE, VK_TAB, reinterpret_cast<LPARAM>(&bodyTab))));
    // SetKeyboardState changes only this test thread's state; it does not inject
    // an input event or alter the user's desktop keyboard state.
    BYTE originalKeys[256]{};
    CheckUi(GetKeyboardState(originalKeys), "Could not inspect the test thread keyboard state.");
    BYTE reverseKeys[256]{}; std::copy(std::begin(originalKeys), std::end(originalKeys), std::begin(reverseKeys));
    reverseKeys[VK_SHIFT] |= 0x80;
    CheckUi(SetKeyboardState(reverseKeys), "Could not set the test thread's reverse-tab modifier.");
    MSG reverseTab{};
    reverseTab.hwnd = GetFocus(); reverseTab.message = WM_KEYDOWN; reverseTab.wParam = VK_TAB;
    const auto reverseConsumed = IsDialogMessageW(app.window, &reverseTab);
    const bool reversedToBody = GetFocus() == Control(app, IDC_OUTPUT);
    CheckUi(SetKeyboardState(originalKeys), "Could not restore the test thread keyboard state.");
    CheckUi(reverseConsumed && reversedToBody, "Shift+Tab from the Roll footer must return to the final body field.");
    SetFocus(Control(app, IDC_STATUS));
    MSG footerTab{};
    footerTab.hwnd = GetFocus(); footerTab.message = WM_KEYDOWN; footerTab.wParam = VK_TAB;
    CheckUi(IsDialogMessageW(app.window, &footerTab) &&
        (GetFocus() == Control(app, IDC_PRESET) || IsChild(Control(app, IDC_PRESET), GetFocus())),
        "Tab from the final footer field must return to the first body field.");

    SetMode(app, dice::Mode::Normal);
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
    CheckUi(IsWindowEnabled(Control(app, IDC_SAVE)) && IsWindowEnabled(Control(app, IDC_ROLL)), "Continuous rolling is blocked by a pending batch.");
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

void UiQuickRollTests(const std::filesystem::path& base) {
    {
        TestWindow test(base);
        auto& app = test.app;
        app.historyRoot = base / L"quick-history";
        SetRoll(app, L"1d1", L"7");
        CheckUi(!Checked(app, IDC_QUICK_IMMEDIATE), "Immediate quick rolling must default to off.");
        SendMessageW(Control(app, IDC_QUICK_FIRST), BM_CLICK, 0, 0);
        CheckUi(!Busy(app) && app.history.empty() && Text(Control(app, IDC_DICE_EDIT)) == L"1d4",
            "A quick die rolled while the switch was off.");
        SendMessageW(Control(app, IDC_QUICK_IMMEDIATE), BM_CLICK, 0, 0);
        CheckUi(Checked(app, IDC_QUICK_IMMEDIATE) && !Busy(app), "Enabling the switch must not itself roll dice.");
        SetMode(app, dice::Mode::Advantage);
        SendMessageW(Control(app, IDC_TARGET), BM_SETCHECK, BST_CHECKED, 0);
        SetWindowTextW(Control(app, IDC_TARGET_VALUE), L"1");
        for (int i = 0; i < 7; ++i) {
            const auto before = app.history.size();
            SendMessageW(Control(app, IDC_QUICK_FIRST + i), BM_CLICK, 0, 0);
            CheckUi(Busy(app) && app.requested == 1 && !IsWindowEnabled(Control(app, IDC_QUICK_IMMEDIATE)),
                "A quick click did not start exactly one roll or lock its option during work.");
            // A queued second command must not alter the in-flight request.
            SendMessageW(app.window, WM_COMMAND, MAKEWPARAM(IDC_QUICK_FIRST + (i + 1) % 7, BN_CLICKED),
                reinterpret_cast<LPARAM>(Control(app, IDC_QUICK_FIRST + (i + 1) % 7)));
            WaitUi(app);
            CheckUi(app.history.size() == before + 1 && IsWindowEnabled(Control(app, IDC_QUICK_IMMEDIATE)),
                "A busy quick click created an extra batch or left the switch disabled.");
            const auto& summary = app.history.back();
            CheckUi(summary.request.notation == L"1d" + std::to_wstring(QuickSides[i]) &&
                summary.request.times == 1 && summary.resultCount == 1 && summary.request.mode == dice::Mode::Advantage &&
                summary.request.target.enabled && summary.request.target.value == 1 && !summary.saved,
                "Immediate quick rolling lost the selected die, one-roll count, mode, target or save policy.");
            const auto record = dice::ReadHistoryEntry(app.historyRoot, summary.id);
            const auto& result = record.batch.results.front();
            CheckUi(result.first.dice.size() == 1 && result.second && result.second->dice.size() == 1 &&
                result.finalValue >= 1 && result.finalValue <= QuickSides[i], "Quick result is outside the selected die's range.");
            CheckUi(Text(Control(app, IDC_TIMES_EDIT)) == L"7" && Checked(app, IDC_RAD_ADV),
                "A quick click changed the regular batch count or mode.");
        }
        // An unfinished batch count is irrelevant to a one-roll quick action.
        SetWindowTextW(Control(app, IDC_TIMES_EDIT), L"");
        SendMessageW(Control(app, IDC_AUTOSAVE), BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(Control(app, IDC_POOL), BM_SETCHECK, BST_CHECKED, 0);
        SetWindowTextW(Control(app, IDC_POOL_VALUE), L"1");
        const auto csv = base / L"quick.csv";
        SetWindowTextW(Control(app, IDC_FILE_EDIT), csv.c_str());
        SendMessageW(Control(app, IDC_QUICK_FIRST), BM_CLICK, 0, 0); WaitUi(app);
        const auto& saved = app.history.back();
        CheckUi(saved.saved && saved.resultCount == 1 && saved.request.spec.successThreshold == 1 &&
            dice::ReadHistoryEntry(app.historyRoot, saved.id).batch.results.front().finalValue == 1 &&
            std::filesystem::exists(csv) && Text(Control(app, IDC_TIMES_EDIT)).empty(),
            "Immediate quick rolling failed to honor pool/automatic export or consumed the batch count.");
        SetWindowTextW(Control(app, IDC_TIMES_EDIT), L"7");
        SendMessageW(Control(app, IDC_QUICK_IMMEDIATE), BM_CLICK, 0, 0);
        const auto before = app.history.size();
        SendMessageW(Control(app, IDC_QUICK_FIRST + 5), BM_CLICK, 0, 0);
        CheckUi(!Busy(app) && app.history.size() == before && Text(Control(app, IDC_DICE_EDIT)) == L"1d20",
            "Disabling the switch did not restore selection-only behavior.");
        SendMessageW(Control(app, IDC_QUICK_IMMEDIATE), BM_CLICK, 0, 0);
        Close(app);
        CheckUi(!IsWindow(app.window), "Could not close after a durable quick roll.");
    }
    {
        TestWindow reopened(base, true);
        auto& app = reopened.app;
        CheckUi(Checked(app, IDC_QUICK_IMMEDIATE) && Text(Control(app, IDC_TIMES_EDIT)) == L"7",
            "Immediate quick-roll preference or regular batch count was not restored.");
        SendMessageW(Control(app, IDC_QUICK_IMMEDIATE), BM_CLICK, 0, 0);
        Close(app);
    }
    {
        TestWindow reopened(base, true);
        CheckUi(!Checked(reopened.app, IDC_QUICK_IMMEDIATE), "Disabling immediate quick rolling did not persist.");
    }
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
                CheckUi(std::wstring(picker->lpstrFile) == L"dice_results_v3.csv", "Invalid path did not fall back to a CSV filename.");
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
        SetMode(app, dice::Mode::Advantage);
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
    GetClientRect(app.body, &viewport);
    MapWindowPoints(app.body, app.window, reinterpret_cast<POINT*>(&viewport), 2);
    CheckUi(app.contentHeight > viewport.bottom, "Small high-DPI window does not expose scrollable content.");
    Scroll(app, SB_VERT, SB_BOTTOM);
    const auto finalContent = Bounds(Control(app, IDC_RETENTION), app.window);
    CheckUi(app.scrollY > 0 && Contains(viewport, finalContent),
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
    SetWindowPos(app.window, nullptr, 0, 0, 420, 900, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    // Windows enforces the physical minimum during SetWindowPos. Simulate a
    // still higher scale in that fixed viewport, as on a narrow high-DPI monitor.
    app.dpi = 384;
    SetFont(app);
    Layout(app);
    GetClientRect(app.body, &viewport);
    MapWindowPoints(app.body, app.window, reinterpret_cast<POINT*>(&viewport), 2);
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
#include "MainLayoutTests.inl"
