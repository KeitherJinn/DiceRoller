// Compile the production feature-window implementation into this test translation
// unit once. The test project must not also compile FeatureWindows.cpp separately.
// All windows stay hidden; messages target only windows created by these tests.
#include "../FeatureWindows.cpp"
#include <chrono>
#include <thread>
#include <fstream>

namespace {
void FeatureCheck(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void WaitCalculation(ui::Panel& panel) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((ui::Working(panel) || panel.detailsPending) && std::chrono::steady_clock::now() < deadline) {
        ui::Tick(panel);
        if (ui::Working(panel)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (ui::Working(panel)) panel.cancel = true;
    FeatureCheck(!ui::Working(panel), "Feature worker failed to complete within ten seconds.");
}
struct HiddenPanel {
    ui::Panel panel;
    ATOM type = 0;
    HiddenPanel(const std::vector<dice::HistoryEntry>* entries, const dice::Request* request = nullptr,
        const std::filesystem::path& root = {}, std::vector<dice::HistorySummary> index = {},
        const std::vector<dice::HistoryEntry>* dirty = nullptr,
        std::vector<dice::HistorySummary> pendingExportMarks = {}) {
        INITCOMMONCONTROLSEX init{sizeof(init), ICC_LISTVIEW_CLASSES};
        FeatureCheck(InitCommonControlsEx(&init) != FALSE, "Feature controls failed to initialize.");
        panel.entries = entries;
        panel.probability = request != nullptr;
        panel.root = root; panel.index = std::move(index); panel.dirty = dirty;
        panel.pendingExportMarks = std::move(pendingExportMarks);
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
        if (!request) WaitCalculation(panel);
    }
    ~HiddenPanel() {
        panel.cancel = true;
        if (panel.future.valid()) panel.future.wait();
        if (panel.historyFuture.valid()) panel.historyFuture.wait();
        if (IsWindow(panel.window)) DestroyWindow(panel.window);
        if (type) UnregisterClassW(MAKEINTATOM(type), GetModuleHandleW(nullptr));
    }
};
void Click(ui::Panel& panel, int id) {
    SendMessageW(panel.window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(ui::Child(panel, id)));
    if (!panel.probability) WaitCalculation(panel);
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
    WaitCalculation(panel);
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
struct FeatureDirectory {
    std::filesystem::path parent=std::filesystem::temp_directory_path(), path;
    FeatureDirectory() {
        GUID id{};FeatureCheck(SUCCEEDED(CoCreateGuid(&id)),"Could not create unique history test directory.");
        wchar_t text[40]{};StringFromGUID2(id,text,40);
        path=parent/(std::wstring(L"DiceRollerFeatureTests-")+text);
        FeatureCheck(std::filesystem::create_directory(path),"Could not create history test directory.");
    }
    ~FeatureDirectory() {
        if(path.parent_path()==parent && path.filename().wstring().find(L"DiceRollerFeatureTests-{")==0){
            std::error_code ignored;std::filesystem::remove_all(path,ignored);
        }
    }
};
void CheckMetadataHistory() {
    using namespace dice;
    std::vector<HistoryEntry> many;
    for(int i=0;i<205;++i)many.push_back(Entry((L"Batch "+std::to_wstring(i)).c_str(),L"d6",{{1}},"2026-10-01T12:00:00.000Z"));
    {
        HiddenPanel window(&many);auto& panel=window.panel;
        FeatureCheck(panel.matches.size()==205 && panel.visible.size()==100 && panel.shown.size()==1 && panel.cache.size()<=2,
            "Metadata list must page at 100 rows and load only selected details.");
        Click(panel,ui::PageNext);
        FeatureCheck(panel.page==1 && panel.visible.front()==100 && panel.visible.size()==100,"Second history page must retain metadata indices.");
        Select(panel,{0,99});
        FeatureCheck(panel.shown.size()==2 && panel.cache.size()==2,"Two-batch comparison must keep a bounded details cache.");
        Click(panel,ui::PageNext);
        FeatureCheck(panel.page==2 && panel.visible.front()==200 && panel.visible.size()==5,"Final history page must show its remaining rows.");
        Click(panel,ui::PagePrevious);
        FeatureCheck(panel.page==1 && panel.shown.front()->entry.id==many[100].id,"Previous page must load the actual selected batch.");
    }
    FeatureDirectory directory;
    auto pending=Entry(L"Pending",L"d6",{{2},{5}},"2026-10-01T12:00:00.000Z");
    auto exported=Entry(L"Exported",L"d20",{{20}},"2026-10-02T12:00:00.000Z",true);
    SaveHistoryEntry(directory.path,pending);SaveHistoryEntry(directory.path,exported);
    {
        HiddenPanel window(nullptr,nullptr,directory.path,ScanHistory(directory.path).entries);auto& panel=window.panel;
        FeatureCheck(panel.shown.front()->entry.id==pending.id && !IsWindowEnabled(ui::Child(panel,ui::Archive)),
            "Pending batches must load from disk and cannot be archived.");
        SetWindowTextW(ui::Child(panel,ui::FilterName),L"Exported");Click(panel,ui::Filter);
        FeatureCheck(IsWindowEnabled(ui::Child(panel,ui::Archive)),"Only durable exported history can be archived.");
        const auto keep=ui::SelectedIds(panel);
        auto external=Entry(L"External",L"d8",{{8}},"2026-10-03T12:00:00.000Z");SaveHistoryEntry(directory.path,external);
        Click(panel,ui::Reload);
        FeatureCheck(panel.selection.indexChanged && panel.index.size()==3 && ui::SelectedIds(panel)==keep &&
            ui::GetText(ui::Child(panel,ui::FilterName))==L"Exported", "Reload must discover external records and preserve valid filters and selection.");
        Click(panel,ui::Archive);
        FeatureCheck(panel.visible.empty() && panel.selection.indexChanged &&
            ReadHistoryEntry(directory.path,exported.id,true).archived,"Archive must move exported records and update the visible metadata index.");
        SendMessageW(ui::Child(panel,ui::IncludeArchived),BM_SETCHECK,BST_CHECKED,0);Click(panel,ui::IncludeArchived);
        FeatureCheck(panel.visible.size()==1 && panel.shown.front()->entry.archived && !IsWindowEnabled(ui::Child(panel,ui::Archive)),
            "Include archived must load archived details while disallowing repeated archive.");
        SetWindowTextW(ui::Child(panel,ui::DateFrom),L"invalid");const auto selected=ui::SelectedIds(panel);
        Click(panel,ui::Reload);
        FeatureCheck(ui::SelectedIds(panel)==selected && panel.visible.size()==1,"Invalid reload filters must not discard the current selection.");
    }
    // An in-memory completed batch must win over its older disk checkpoint.
    auto checkpoint=pending;checkpoint.batch.results.resize(1);
    auto dirty=Entry(L"Dirty",L"d6",{{3},{6}},"2026-10-04T12:00:00.000Z");
    checkpoint=dirty;checkpoint.batch.results.resize(1);SaveHistoryEntry(directory.path,checkpoint);
    std::vector<HistoryEntry> dirtyEntries{dirty};
    {
        HiddenPanel window(nullptr,nullptr,directory.path,{SummaryOf(checkpoint)},&dirtyEntries);auto& panel=window.panel;
        FeatureCheck(panel.index.front().resultCount==2 && !panel.index.front().localPersisted && panel.shown.front()->entry.batch.results.size()==2,
            "Memory-only completed results must override an older on-disk checkpoint.");
        Click(panel,ui::Next);
        FeatureCheck(ui::GetText(ui::Child(panel,ui::Detail)).find(L"final: 6")!=std::wstring::npos && !IsWindowEnabled(ui::Child(panel,ui::Archive)),
            "Dirty fallback must expose every completed roll and forbid archive.");
    }
    auto marked=Entry(L"Export status",L"d6",{{4}},"2026-10-05T12:00:00.000Z",true);
    marked.exportedPath=L"C:\\original.csv";SaveHistoryEntry(directory.path,marked);
    auto pendingMark=SummaryOf(marked);pendingMark.exportedPath=L"C:\\new-destination.csv";
    {
        HiddenPanel window(nullptr,nullptr,directory.path,{SummaryOf(marked)},nullptr,{pendingMark});auto& panel=window.panel;
        SetWindowTextW(ui::Child(panel,ui::FilterName),L"Export status");Click(panel,ui::Filter);Click(panel,ui::Reload);
        FeatureCheck(panel.visible.size()==1 && panel.index[panel.visible[0]].exportedPath==pendingMark.exportedPath &&
            panel.index[panel.visible[0]].localPersisted && ui::GetText(ui::Child(panel,ui::Detail)).find(pendingMark.exportedPath)!=std::wstring::npos,
            "Reload and details must preserve pending export status without marking durable dice as memory-only.");
        FeatureCheck(!IsWindowEnabled(ui::Child(panel,ui::Archive)),"Pending export markers must disable archive.");
        const auto before=ui::SelectedIds(panel);Click(panel,ui::Archive);
        FeatureCheck(ui::SelectedIds(panel)==before && !ReadHistoryEntry(directory.path,marked.id).archived &&
            ui::GetText(ui::Child(panel,ui::HistoryStatus)).find(L"Retry saving")!=std::wstring::npos,
            "A direct archive command must also reject pending export status while preserving the record and selection.");
    }
    {
        const auto active=directory.path/(Wide(marked.id)+L".dhist"), archived=directory.path/L"Archive"/(Wide(marked.id)+L".dhist");
        std::filesystem::copy_file(active,archived);
        HiddenPanel window(nullptr,nullptr,directory.path,{SummaryOf(marked)});auto& panel=window.panel;
        SetWindowTextW(ui::Child(panel,ui::FilterName),L"Export status");Click(panel,ui::Filter);
        const auto selected=ui::SelectedIds(panel);Click(panel,ui::Archive);
        FeatureCheck(ui::SelectedIds(panel)==selected && std::filesystem::exists(active) && std::filesystem::exists(archived) &&
            ui::GetText(ui::Child(panel,ui::HistoryStatus)).find(L"Archive stopped after 0")!=std::wstring::npos,
            "Archive conflicts must preserve both original files, selection, filters, and the failure notice after details load.");
    }
    auto damaged=Entry(L"Damaged payload",L"d6",{{2}},"2026-10-06T12:00:00.000Z");SaveHistoryEntry(directory.path,damaged);
    const auto damagedPath=directory.path/(Wide(damaged.id)+L".dhist");
    {
        std::fstream stream(damagedPath,std::ios::in|std::ios::out|std::ios::binary);
        FeatureCheck(stream.good(),"Could not open the owned damaged-payload fixture.");
        // The final value precedes the 4-byte timestamp length and 19-byte timestamp.
        stream.seekp(-31,std::ios::end);stream.put(static_cast<char>(127));
    }
    {
        HiddenPanel window(nullptr,nullptr,directory.path,{SummaryOf(damaged)});auto& panel=window.panel;
        FeatureCheck(panel.visible.size()==1 && ui::SelectedIds(panel)==std::vector<std::string>{damaged.id} && panel.shown.empty() &&
            ui::GetText(ui::Child(panel,ui::HistoryStatus)).find(L"Could not load history")!=std::wstring::npos && std::filesystem::exists(damagedPath),
            "A corrupt detail payload must report failure without removing its metadata selection or original file.");
    }
}
void CheckSmallDpi(ui::Panel& panel) {
    for (UINT dpi : {96u, 144u, 192u, 288u}) {
        RECT suggested{0, 0, MulDiv(360, dpi, 96), MulDiv(600, dpi, 96)};
        SendMessageW(panel.window, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&suggested));
        RECT client{}, body{};
        GetClientRect(panel.window, &client); GetClientRect(panel.body, &body);
        FeatureCheck(panel.contentWidth == (std::max)(static_cast<int>(body.right), ui::Scale(panel, 320)),
            "Feature body must adapt to the viewport, retaining only a 320 DIP fallback width.");
        FeatureCheck(panel.contentHeight > body.bottom && panel.footerHeight < client.bottom,
            "Body must scroll independently above a visible footer.");
        LOGFONTW font{};
        GetObjectW(panel.font, sizeof(font), &font);
        FeatureCheck(font.lfHeight == -MulDiv(10, static_cast<int>(dpi), 72), "Feature font must follow monitor DPI.");
        for (HWND child = GetWindow(panel.body, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            RECT bounds{}; GetWindowRect(child, &bounds);
            MapWindowPoints(nullptr, panel.body, reinterpret_cast<POINT*>(&bounds), 2);
            OffsetRect(&bounds, panel.scrollX, panel.scrollY);
            FeatureCheck(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= panel.contentWidth && bounds.bottom <= panel.contentHeight,
                "Body control must remain inside its reachable virtual content area.");
            wchar_t name[32]{}; GetClassNameW(child, name, 32);
            if (std::wstring(name) == L"Static" && GetDlgCtrlID(child) != ui::Chart) {
                RECT measured{0,0,bounds.right-bounds.left,0}; const auto text=ui::GetText(child);
                auto dc=GetDC(child);auto previous=SelectObject(dc,panel.font);
                DrawTextW(dc,text.c_str(),-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
                SelectObject(dc,previous);ReleaseDC(child,dc);
                FeatureCheck(measured.bottom <= bounds.bottom-bounds.top,
                    "Actual scaled text must fit the explanation/label height, not only control bounds.");
            }
        }
        const auto before = panel.scrollY;
        ui::ScrollPanel(panel, SB_VERT, SB_BOTTOM);
        FeatureCheck(panel.scrollY >= before, "Body scroll must move without shifting footer actions.");
        for (int id : panel.probability ? std::vector<int>{ui::Calculate,ui::CancelCalculation,ui::Close} :
            std::vector<int>{ui::Reuse,ui::Export,ui::Archive,ui::CancelCalculation,ui::Close}) {
            RECT bounds{}; GetWindowRect(ui::Child(panel,id),&bounds);
            MapWindowPoints(nullptr,panel.window,reinterpret_cast<POINT*>(&bounds),2);
            FeatureCheck(bounds.left>=0 && bounds.top>=body.bottom && bounds.right<=client.right && bounds.bottom<=client.bottom,
                "Every footer action must remain visible at 100-300 percent DPI while the body scrolls.");
            FeatureCheck(bounds.right-bounds.left>=ui::ButtonWidth(panel,id), "Footer labels must fit measured text width.");
        }
        SetFocus(ui::Child(panel,panel.probability?ui::FormulaA:ui::RollNumber));ui::EnsurePanelFocus(panel);
        RECT focus{};GetWindowRect(GetFocus(),&focus);MapWindowPoints(nullptr,panel.body,reinterpret_cast<POINT*>(&focus),2);
        FeatureCheck(focus.top>=0 && focus.bottom<=body.bottom, "Keyboard focus must reveal its edit field in the scrollable body.");
        SetFocus(ui::Child(panel,ui::Close));ui::EnsurePanelFocus(panel);
        FeatureCheck(!IsWindowVisible(panel.window), "DPI and keyboard checks must never show test windows.");
    }
}
void CheckResizeStability(ui::Panel& panel) {
    const auto resize=[&](int width) {
        SetWindowPos(panel.window,nullptr,0,0,ui::Scale(panel,width),ui::Scale(panel,560),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    };
    resize(900);
    SetFocus(ui::Child(panel,panel.probability?ui::FormulaA:ui::FilterName));
    ui::ScrollPanel(panel,SB_VERT,SB_BOTTOM);
    const auto position=panel.scrollY;
    FeatureCheck(position>0,"Resize test must begin while reading below the focused edit field.");
    for(int width:{901,920,980,940,900}) {
        resize(width);
        FeatureCheck(panel.scrollY==position,
            "Resizing must preserve the reading position instead of scrolling back to an unchanged input focus.");
    }
    // Real keyboard navigation still reveals its destination after a resize.
    ui::EnsurePanelFocus(panel);
    RECT focus{},body{};GetWindowRect(GetFocus(),&focus);
    MapWindowPoints(nullptr,panel.body,reinterpret_cast<POINT*>(&focus),2);GetClientRect(panel.body,&body);
    FeatureCheck(focus.top>=0 && focus.bottom<=body.bottom,
        "Revealing actual keyboard focus must still work after preserving a resize scroll position.");
    SetFocus(ui::Child(panel,ui::Close));
    for(int width:{318,319,320,321,322,620,621,622,900,320,900}) {
        resize(width);
        RECT viewport{};GetClientRect(panel.body,&viewport);
        const auto scrollX=panel.scrollX,scrollY=panel.scrollY,contentWidth=panel.contentWidth,contentHeight=panel.contentHeight;
        ui::LayoutPanel(panel);
        FeatureCheck(panel.scrollX==scrollX && panel.scrollY==scrollY && panel.contentWidth==contentWidth && panel.contentHeight==contentHeight,
            "A resize crossing scrollbar and wrapping thresholds must settle without needing another layout pass.");
        SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(panel.body,SB_VERT,&info);
        FeatureCheck(info.nPos==panel.scrollY && info.nPage==static_cast<UINT>(viewport.bottom),
            "Scroll range must match the final viewport throughout a continuous resize.");
    }
    FeatureCheck(!IsWindowVisible(panel.window),"Resize regressions must leave the test window hidden.");
}
}

void FeatureWindowTests() {
    CheckMetadataHistory();
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
        const auto builds = panel.statisticsBuilds;
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
        FeatureCheck(panel.statisticsBuilds == builds, "Roll paging must reuse cached statistics.");
        SetWindowTextW(ui::Child(panel, ui::RollNumber), L"2junk");
        SendMessageW(panel.window, WM_COMMAND, MAKEWPARAM(ui::RollNumber, EN_KILLFOCUS), reinterpret_cast<LPARAM>(ui::Child(panel, ui::RollNumber)));
        FeatureCheck(panel.roll == 1 && ui::GetText(ui::Child(panel,ui::RollNumber))==L"2" &&
            ui::GetText(ui::Child(panel,ui::HistoryStatus)).find(L"complete roll number")!=std::wstring::npos,
            "Roll number must reject a partially numeric string and preserve the current roll.");

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
        Select(panel,{1});
        const auto kept=ui::SelectedIds(panel);
        SetFocus(ui::Child(panel,ui::RollNumber));
        SendMessageW(panel.window,WM_COMMAND,IDOK,0);
        FeatureCheck(ui::SelectedIds(panel)==kept && panel.shown.front()->entry.id==entries[1].id,
            "Enter in Roll number must keep the selected batch instead of reapplying filters.");
        Select(panel, {0, 1});
        const auto summary = ui::GetText(ui::Child(panel, ui::Summary));
        FeatureCheck(summary.find(L"A: Alpha") != std::wstring::npos && summary.find(L"B: Beta") != std::wstring::npos &&
            summary.find(L"target >= 10: 1/2") != std::wstring::npos, "Selected batch comparison must include both summaries and sample success counts.");
        FeatureCheck(!IsWindowEnabled(ui::Child(panel, ui::Reuse)) && IsWindowEnabled(ui::Child(panel, ui::Export)),
            "Multiple selection permits export but must not silently choose parameters to reuse.");
        CheckChart(panel);
        CheckResizeStability(panel);
        FeatureCheck(ui::GetText(ui::Child(panel,ui::Frequencies)).find(L"9\t1\t50.000000")!=std::wstring::npos,
            "The copyable frequency table must expose exact value counts and percentages.");
        CheckSmallDpi(panel);
        panel.exportPreflight=[]{throw std::invalid_argument("Choose a writable CSV destination.");};
        const auto selectedBefore=ui::SelectedIds(panel);
        Click(panel,ui::Export);
        FeatureCheck(IsWindow(panel.window) && ui::SelectedIds(panel)==selectedBefore &&
            ui::GetText(ui::Child(panel,ui::HistoryStatus)).find(L"writable CSV")!=std::wstring::npos,
            "Export destination validation must keep history selection and filters available on failure.");
        panel.exportPreflight={};
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
            IsWindowEnabled(ui::Child(panel, ui::CancelCalculation)) && GetFocus()==ui::Child(panel,ui::CancelCalculation),
            "Probability must run asynchronously and focus its cancellation action.");
        WaitCalculation(panel);
        const auto text = ui::GetText(ui::Child(panel, ui::ProbabilityText));
        FeatureCheck(text.find(L"EXACT") != std::wstring::npos && text.find(L"30.000%") != std::wstring::npos &&
            text.find(L"51.000%") != std::wstring::npos, "Comparison must display the exact normal and advantage target probabilities.");
        FeatureCheck(GetFocus()==ui::Child(panel,ui::Calculate) && text.find(L"d20 [Normal]")!=std::wstring::npos,
            "Completed output must identify the calculated request and restore focus to Calculate.");
        CheckChart(panel);
        CheckResizeStability(panel);
        CheckSmallDpi(panel);
        SetWindowTextW(ui::Child(panel,ui::FormulaA),L"d6");
        FeatureCheck(panel.stale && panel.distributionA.empty() && ui::GetText(ui::Child(panel,ui::Frequencies)).empty() &&
            ui::GetText(ui::Child(panel,ui::ProbabilityText)).find(L"Parameters changed")!=std::wstring::npos,
            "Editing a probability formula must invalidate old text, chart, and frequency table.");
        SetWindowTextW(ui::Child(panel,ui::FormulaA),L"d20");
        Click(panel,ui::Calculate);WaitCalculation(panel);
        SendMessageW(ui::Child(panel,ui::ModeB),CB_SETCURSEL,static_cast<int>(Mode::Normal),0);
        SendMessageW(panel.window,WM_COMMAND,MAKEWPARAM(ui::ModeB,CBN_SELCHANGE),reinterpret_cast<LPARAM>(ui::Child(panel,ui::ModeB)));
        FeatureCheck(panel.stale && panel.distributionB.empty(), "Changing comparison mode must invalidate its old distribution.");
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
