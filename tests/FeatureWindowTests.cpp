// Compile the production feature-window implementation into this test translation
// unit once. The test project must not also compile FeatureWindows.cpp separately.
// All windows stay hidden; messages target only windows created by these tests.
#include "../FeatureWindows.cpp"
#include <chrono>
#include <thread>

namespace {
void FeatureCheck(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct HiddenPanel {
    ui::Panel panel;
    ATOM type = 0;
    HiddenPanel(const std::vector<dice::HistoryEntry>* entries, const dice::Request* request = nullptr) {
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_LISTVIEW_CLASSES};
        FeatureCheck(InitCommonControlsEx(&init) != FALSE, "Feature controls failed to initialize.");
        panel.entries = entries;
        panel.probability = request != nullptr;
        if (request) panel.request = *request;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = ui::PanelProc;
        wc.lpszClassName = L"DiceRollerHiddenFeatureTests";
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        type = RegisterClassExW(&wc);
        FeatureCheck(type != 0, "Could not register hidden feature test window.");
        const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Hidden feature test",
            WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL, 0, 0, 900, 960,
            nullptr, nullptr, wc.hInstance, &panel);
        FeatureCheck(window != nullptr && !IsWindowVisible(window), "Feature test window must be created hidden.");
    }
    ~HiddenPanel() {
        panel.cancel = true;
        if (panel.future.valid()) panel.future.wait();
        if (IsWindow(panel.window)) DestroyWindow(panel.window);
        if (type) UnregisterClassW(MAKEINTATOM(type), GetModuleHandleW(nullptr));
    }
};
void Click(ui::Panel& panel, int id) {
    SendMessageW(panel.window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(ui::Child(panel, id)));
}
void WaitCalculation(ui::Panel& panel) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (panel.future.valid() && std::chrono::steady_clock::now() < deadline) {
        ui::Tick(panel);
        if (panel.future.valid()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (panel.future.valid()) panel.cancel = true;
    FeatureCheck(!panel.future.valid(), "Feature calculation failed to complete within ten seconds.");
}
dice::HistoryEntry Entry(const wchar_t* name, const wchar_t* notation,
    std::vector<std::vector<int>> rolls, const char* created, bool saved = false) {
    dice::Batch batch;
    batch.request.name = name;
    batch.request.notation = notation;
    batch.request.spec = dice::Parse(notation);
    batch.request.times = static_cast<int>(rolls.size());
    for (const auto& values : rolls) {
        std::size_t index = 0;
        auto result = dice::Roll(batch.request.spec, batch.request.mode, [&](int) { return values.at(index++); });
        result.timestamp = "2026-10-28 12:00:00";
        batch.results.push_back(std::move(result));
    }
    auto entry = dice::NewHistoryEntry(std::move(batch));
    entry.created = created;
    entry.saved = saved;
    if (saved) entry.exportedPath = L"C:\\dice-test-results.csv";
    return entry;
}
void Select(ui::Panel& panel, const std::vector<int>& rows) {
    auto list = ui::Child(panel, ui::Batches);
    panel.refreshing = true;
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    for (int row : rows) ListView_SetItemState(list, row, LVIS_SELECTED, LVIS_SELECTED);
    panel.refreshing = false;
    ui::RefreshDetails(panel, true);
}
void CheckChart(ui::Panel& panel) {
    const auto dc = CreateCompatibleDC(nullptr);
    FeatureCheck(dc != nullptr, "Could not create a chart test DC.");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 800;
    info.bmiHeader.biHeight = -320;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) { DeleteDC(dc); throw std::runtime_error("Could not create chart test bitmap."); }
    const auto previous = SelectObject(dc, bitmap);
    DRAWITEMSTRUCT draw{};
    draw.CtlType = ODT_STATIC;
    draw.CtlID = ui::Chart;
    draw.hwndItem = ui::Child(panel, ui::Chart);
    draw.hDC = dc;
    draw.rcItem = {0, 0, 800, 320};
    const auto painted = SendMessageW(panel.window, WM_DRAWITEM, ui::Chart, reinterpret_cast<LPARAM>(&draw));
    GdiFlush();
    bool blue = false, orange = false;
    for (int y = 40; y < 280 && (!blue || !orange); ++y) {
        for (int x = 8; x < 792 && (!blue || !orange); ++x) {
            const auto color = GetPixel(dc, x, y);
            blue |= color == RGB(30, 105, 190);
            orange |= color == RGB(210, 116, 24);
        }
    }
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    FeatureCheck(painted && blue && orange, "Native chart painting must draw both selected distributions.");
}
void CheckSmallDpi(ui::Panel& panel) {
    for (UINT dpi : {96u, 144u, 192u}) {
        RECT suggested{0, 0, 420, 340};
        SendMessageW(panel.window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
        RECT client{};
        GetClientRect(panel.window, &client);
        FeatureCheck(panel.contentWidth >= ui::Scale(panel, 760) && panel.contentHeight >= ui::Scale(panel, panel.probability ? 690 : 850),
            "Small feature windows must retain scrollable content at every tested DPI.");
        LOGFONTW font{};
        GetObjectW(panel.font, sizeof(font), &font);
        FeatureCheck(font.lfHeight == -MulDiv(10, static_cast<int>(dpi), 72), "Feature font must follow monitor DPI.");
        for (HWND child = GetWindow(panel.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            RECT bounds{};
            GetWindowRect(child, &bounds);
            MapWindowPoints(nullptr, panel.window, reinterpret_cast<POINT*>(&bounds), 2);
            OffsetRect(&bounds, panel.scrollX, panel.scrollY);
            FeatureCheck(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= panel.contentWidth && bounds.bottom <= panel.contentHeight,
                "Feature control must remain inside its reachable virtual content area.");
        }
        SetFocus(ui::Child(panel, ui::Close));
        ui::EnsurePanelFocus(panel);
        RECT close{};
        GetWindowRect(ui::Child(panel, ui::Close), &close);
        MapWindowPoints(nullptr, panel.window, reinterpret_cast<POINT*>(&close), 2);
        FeatureCheck(close.left >= 0 && close.top >= 0 && close.right <= client.right && close.bottom <= client.bottom,
            "Keyboard focus must scroll the Close button into view in a small feature window.");
        SetFocus(ui::Child(panel, panel.probability ? ui::FormulaA : ui::FilterName));
        ui::EnsurePanelFocus(panel);
    }
}
}

void FeatureWindowTests() {
    using namespace dice;
    std::vector<HistoryEntry> entries{
        Entry(L"Alpha", L"2d6+1d4+3", {{1, 2, 3}, {4, 5, 4}}, "2026-10-01T12:00:00.000Z"),
        Entry(L"Beta", L"4d6kh3", {{1, 4, 6, 3}}, "2026-10-02T12:00:00.000Z"),
        Entry(L"Gamma", L"d20", {{20}}, "2026-10-03T12:00:00.000Z", true)
    };
    entries.front().batch.request.target = {true, Compare::AtLeast, 10};
    {
        HiddenPanel window(&entries);
        auto& panel = window.panel;
        FeatureCheck(ListView_GetItemCount(ui::Child(panel, ui::Batches)) == 3, "History should initially show all batches.");
        FeatureCheck(ui::GetText(ui::Child(panel, ui::Detail)).find(L"roll 1") != std::wstring::npos,
            "Initial history selection must show full first-roll details.");
        Click(panel, ui::Next);
        FeatureCheck(panel.roll == 1 && ui::GetText(ui::Child(panel, ui::Detail)).find(L"final: 16") != std::wstring::npos,
            "Next must page to the actual second result.");
        FeatureCheck(!IsWindowEnabled(ui::Child(panel, ui::Next)) && IsWindowEnabled(ui::Child(panel, ui::Previous)),
            "Paging buttons must reflect result boundaries.");
        Click(panel, ui::Previous);
        FeatureCheck(panel.roll == 0 && !IsWindowEnabled(ui::Child(panel, ui::Previous)), "Previous must return to the first result.");
        SetWindowTextW(ui::Child(panel, ui::RollNumber), L"2");
        SendMessageW(panel.window, WM_COMMAND, MAKEWPARAM(ui::RollNumber, EN_KILLFOCUS), reinterpret_cast<LPARAM>(ui::Child(panel, ui::RollNumber)));
        FeatureCheck(panel.roll == 1, "Manual roll-number navigation must show the requested result.");

        SetWindowTextW(ui::Child(panel, ui::FilterName), L"beta");
        Click(panel, ui::Filter);
        FeatureCheck(panel.visible == std::vector<std::size_t>{1} &&
            ui::GetText(ui::Child(panel, ui::Detail)).find(L"[1 dropped]") != std::wstring::npos,
            "Case-insensitive filtering must retain original indices and show dropped dice.");
        SetWindowTextW(ui::Child(panel, ui::FilterName), L"");
        SetWindowTextW(ui::Child(panel, ui::FilterDice), L"d20");
        Click(panel, ui::Filter);
        FeatureCheck(panel.visible == std::vector<std::size_t>{2}, "Expression filtering must select the matching batch.");
        SetWindowTextW(ui::Child(panel, ui::FilterDice), L"");
        SetWindowTextW(ui::Child(panel, ui::DateFrom), L"2026-10-02");
        SetWindowTextW(ui::Child(panel, ui::DateTo), L"2026-10-03");
        SendMessageW(ui::Child(panel, ui::Unsaved), BM_SETCHECK, BST_CHECKED, 0);
        Click(panel, ui::Filter);
        FeatureCheck(panel.visible == std::vector<std::size_t>{1}, "Date and pending-export filters must combine correctly.");
        SetWindowTextW(ui::Child(panel, ui::FilterName), L"missing");
        Click(panel, ui::Filter);
        FeatureCheck(panel.visible.empty() && !IsWindowEnabled(ui::Child(panel, ui::Reuse)) && !IsWindowEnabled(ui::Child(panel, ui::Export)),
            "Empty filtered history must disable actions needing a selection.");
        SetWindowTextW(ui::Child(panel, ui::FilterName), L"");
        SetWindowTextW(ui::Child(panel, ui::DateFrom), L"");
        SetWindowTextW(ui::Child(panel, ui::DateTo), L"");
        SendMessageW(ui::Child(panel, ui::Unsaved), BM_SETCHECK, BST_UNCHECKED, 0);
        Click(panel, ui::Filter);
        Select(panel, {0, 1});
        const auto summary = ui::GetText(ui::Child(panel, ui::Summary));
        FeatureCheck(summary.find(L"A: Alpha") != std::wstring::npos && summary.find(L"B: Beta") != std::wstring::npos &&
            summary.find(L"target >= 10: 1/2") != std::wstring::npos, "Selected batch comparison must include both summaries and sample success counts.");
        FeatureCheck(!IsWindowEnabled(ui::Child(panel, ui::Reuse)) && IsWindowEnabled(ui::Child(panel, ui::Export)),
            "Multiple selection permits export but must not silently choose parameters to reuse.");
        CheckChart(panel);
        CheckSmallDpi(panel);
        Click(panel, ui::Export);
        FeatureCheck(!IsWindow(panel.window) && panel.selection.action == ui::HistorySelection::Action::Export &&
            panel.selection.indices == std::vector<std::size_t>({0, 1}), "History export must return all selected original batch indices.");
    }
    {
        HiddenPanel window(&entries);
        Select(window.panel, {2});
        Click(window.panel, ui::Reuse);
        FeatureCheck(window.panel.selection.action == ui::HistorySelection::Action::Reuse &&
            window.panel.selection.indices == std::vector<std::size_t>{2}, "Reuse must return the chosen batch, without rolling new dice.");
    }
    Request request{L"Probability check", L"d20", Parse(L"d20"), Mode::Normal, 1};
    request.target = {true, Compare::AtLeast, 15};
    {
        HiddenPanel window(nullptr, &request);
        auto& panel = window.panel;
        SendMessageW(ui::Child(panel, ui::ModeB), CB_SETCURSEL, static_cast<int>(Mode::Advantage), 0);
        Click(panel, ui::Calculate);
        FeatureCheck(panel.future.valid() && !IsWindowEnabled(ui::Child(panel, ui::Calculate)) &&
            IsWindowEnabled(ui::Child(panel, ui::CancelCalculation)), "Probability must run asynchronously with appropriate controls.");
        WaitCalculation(panel);
        const auto text = ui::GetText(ui::Child(panel, ui::ProbabilityText));
        FeatureCheck(text.find(L"EXACT") != std::wstring::npos && text.find(L"30.000%") != std::wstring::npos &&
            text.find(L"51.000%") != std::wstring::npos, "Comparison must display the exact normal and advantage target probabilities.");
        CheckChart(panel);
        CheckSmallDpi(panel);
        SetWindowTextW(ui::Child(panel, ui::FormulaA), L"1000d1000");
        SetWindowTextW(ui::Child(panel, ui::FormulaB), L"1000d1000");
        Click(panel, ui::Calculate);
        Click(panel, ui::CancelCalculation);
        WaitCalculation(panel);
        FeatureCheck(ui::GetText(ui::Child(panel, ui::ProbabilityText)).find(L"cancelled") != std::wstring::npos &&
            IsWindowEnabled(ui::Child(panel, ui::Calculate)) && !IsWindowEnabled(ui::Child(panel, ui::CancelCalculation)),
            "Cancelling a probability calculation must return to an actionable state.");
        Click(panel, ui::Calculate);
        SendMessageW(panel.window, WM_CLOSE, 0, 0);
        WaitCalculation(panel);
        FeatureCheck(!IsWindow(panel.window), "Closing during calculation must cancel and destroy the feature window after the worker finishes.");
    }
}
