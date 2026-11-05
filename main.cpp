#include "framework.h"
#include "Resource.h"
#include "RollJob.h"
#include "AppSettings.h"
#include "WindowLayout.h"
#include "FeatureWindows.h"
#include "SingleInstance.h"
#include <future>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Comctl32.lib")

namespace {
constexpr UINT_PTR PollTimer = 1;
constexpr UINT FocusChanged = WM_APP + 1;
// The body is a real child viewport, so its controls cannot draw over the footer.
// Group boxes live inside that body, whose background remains normally erased.
constexpr DWORD MainStyle = WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN;
constexpr int QuickSides[] = {4, 6, 8, 10, 12, 20, 100};
struct App {
    HWND window = nullptr;
    HWND body = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    int defaultButton = IDC_ROLL;
    int requested = 0;
    bool saveOnly = false;
    bool closing = false;
    bool logFailed = false;
    bool closePrompt = false;
    bool settingsNeedBackup = false;
    bool layingOut = false;
    bool historyLoading = false;
    bool flushingClose = false;
    bool closeFlushAttempted = false;
    std::atomic<bool> cancelIndex{false};
    std::future<dice::HistoryIndex> historyScan;
    int scrollX = 0, scrollY = 0;
    int contentWidth = 0, contentHeight = 0;
    int bodyHeight = 0;
    int wheelRemainder = 0;
    std::filesystem::path base;
    std::filesystem::path settingsPath;
    std::filesystem::path historyRoot;
    std::vector<dice::HistorySummary> history;
    std::vector<dice::HistoryEntry> dirtyHistory;
    std::vector<dice::HistorySummary> pendingMarks;
    dice::AppSettings settings;
    std::function<int(HWND, LPCWSTR, LPCWSTR, UINT)> confirm = MessageBoxW;
    std::function<BOOL(OPENFILENAMEW*)> filePicker = GetSaveFileNameW;
    std::shared_ptr<const dice::Batch> pending;
    dice::LogBuffer log;
    dice::RollJob job;
    ~App() { cancelIndex = true; if (historyScan.valid()) historyScan.wait(); if (font) DeleteObject(font); }
};
HWND Control(const App& app, int id) {
    const auto control = GetDlgItem(app.window, id);
    return control ? control : GetDlgItem(app.body, id);
}
bool Checked(const App& app, int id) { return SendMessageW(Control(app, id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
void SetMode(App& app, dice::Mode mode) {
    CheckRadioButton(app.body, IDC_RAD_NORMAL, IDC_RAD_DISADV, IDC_RAD_NORMAL + static_cast<int>(mode));
}
bool Busy(const App& app) { return app.historyLoading || app.job.State() != dice::Phase::Idle; }
bool HasPending(const App& app) {
    if (app.historyRoot.empty()) return static_cast<bool>(app.pending);
    return std::any_of(app.history.begin(), app.history.end(), [](const auto& e) { return !e.saved && e.resultCount; });
}
int Px(const App& app, int value) { return MulDiv(value, static_cast<int>(app.dpi), 96); }
void Layout(App& app);
void EnsureFocusVisible(App& app);
bool FinishClose(App& app);
void RefreshPresets(App& app, const std::wstring& selected = {}, bool rebuildGroups = true);
void RestoreRequest(App& app, const dice::Request& request);
dice::Request ReadRequest(const App& app, bool singleRoll = false);
void StartRoll(App& app, bool singleRoll = false);
void History(App& app);
void ExchangePresets(App& app, bool importing);

std::wstring Text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(copied);
    return text;
}
void Status(App& app, const std::wstring& text) {
    if (Text(Control(app, IDC_STATUS)) == text) return;
    SetWindowTextW(Control(app, IDC_STATUS), text.c_str());
    Layout(app);
}
void Error(HWND window, const std::exception& error) {
    std::wstring message;
    try { message = dice::Wide(error.what()); }
    catch (...) { message = L"Operation failed."; }
    MessageBoxW(window, message.c_str(), L"Dice Roller", MB_OK | MB_ICONERROR);
}

void EnableActions(App& app) {
    const bool busy = Busy(app);
    for (int id : {IDC_FILE_EDIT, IDC_NAME_EDIT, IDC_DICE_EDIT, IDC_TIMES_EDIT,
                   IDC_RAD_NORMAL, IDC_RAD_ADV, IDC_RAD_DISADV, IDC_TIMESTAMP,
                   IDC_AUTOSAVE, IDC_BROWSE, IDC_OPEN, IDC_PRESET, IDC_PRESET_APPLY,
                   IDC_PRESET_SAVE, IDC_PRESET_DELETE, IDC_GROUP, IDC_IMPORT, IDC_EXPORT_PRESETS,
                   IDC_TARGET, IDC_COMPARISON, IDC_TARGET_VALUE, IDC_POOL, IDC_POOL_VALUE, IDC_PROBABILITY, IDC_QUICK_IMMEDIATE})
        EnableWindow(Control(app, id), !busy);
    for (int id = IDC_QUICK_FIRST; id <= IDC_QUICK_LAST; ++id) EnableWindow(Control(app, id), !busy);
    EnableWindow(Control(app, IDC_ROLL), !busy);
    EnableWindow(Control(app, IDC_CANCEL), busy && !app.closing && !app.historyLoading);
    EnableWindow(Control(app, IDC_SAVE), !busy && HasPending(app));
    EnableWindow(Control(app, IDC_DISCARD), !busy && !app.historyRoot.empty());
    const int focus = busy ? IDC_CANCEL : IDC_ROLL;
    if (!GetFocus() || !IsChild(app.window, GetFocus()) || !IsWindowEnabled(GetFocus()))
        SetFocus(Control(app, focus));
    SendMessageW(app.window, DM_SETDEFID, focus, 0);
    EnsureFocusVisible(app);
}

void RefreshLog(App& app) {
    const auto text = app.log.Text();
    app.logFailed = false;
    if (!SetWindowTextW(Control(app, IDC_OUTPUT), text.c_str())) app.logFailed = true;
    SendMessageW(Control(app, IDC_OUTPUT), EM_SETSEL, text.size(), text.size());
    SendMessageW(Control(app, IDC_OUTPUT), EM_SCROLLCARET, 0, 0);
    const std::wstring label = app.logFailed ? L"The log could not be fully displayed; the batch is still retained."
        : L"Display: latest 500 entries / 120,000 characters. Older entries removed: " + std::to_wstring(app.log.Dropped())
          + L". Complete batches remain in History.";
    SetWindowTextW(Control(app, IDC_RETENTION), label.c_str());
    Layout(app);
}

#include "MainLayout.inl"

void Scroll(App& app, int bar, UINT action) {
    SCROLLINFO info{sizeof(info), SIF_ALL};
    GetScrollInfo(app.window, bar, &info);
    int position = info.nPos;
    switch (action) {
    case SB_LINEUP: position -= Px(app, 32); break;
    case SB_LINEDOWN: position += Px(app, 32); break;
    case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
    case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: position = info.nTrackPos; break;
    case SB_TOP: position = info.nMin; break;
    case SB_BOTTOM: position = info.nMax; break;
    default: return;
    }
    (bar == SB_VERT ? app.scrollY : app.scrollX) = position;
    Layout(app);
}

LRESULT CALLBACK FocusProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                 UINT_PTR, DWORD_PTR parent) {
    if (message == WM_SETFOCUS) PostMessageW(reinterpret_cast<HWND>(parent), FocusChanged, 0, 0);
    auto result = DefSubclassProc(window, message, wParam, lParam);
    // Read-only multiline edits request all keys by default. Let dialog navigation
    // handle Tab/Shift+Tab so the log and status do not trap keyboard focus.
    if (message == WM_GETDLGCODE && wParam == VK_TAB &&
        (GetDlgCtrlID(window) == IDC_OUTPUT || GetDlgCtrlID(window) == IDC_STATUS))
        result &= ~(DLGC_WANTALLKEYS | DLGC_WANTTAB);
    return result;
}

void SetFont(App& app) {
    const auto font = CreateFontW(-MulDiv(10, static_cast<int>(app.dpi), 72), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!font) throw std::runtime_error("Could not create the interface font.");
    for (HWND parent : {app.window, app.body})
        for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (app.font) DeleteObject(app.font);
    app.font = font;
}

LRESULT CALLBACK MainBodyProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // Native notifications go to the immediate parent; commands still belong to
    // the main window, including keyboard/mouse scrolling and owner-drawn items.
    switch (message) {
    case WM_COMMAND: case WM_NOTIFY: case WM_DRAWITEM:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        return SendMessageW(GetParent(window), message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
void Setup(App& app) {
    WNDCLASSEXW bodyClass{sizeof(bodyClass)};
    bodyClass.hInstance = GetModuleHandleW(nullptr);
    bodyClass.lpfnWndProc = MainBodyProc;
    bodyClass.lpszClassName = L"DiceRollerMainBody";
    bodyClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    bodyClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassExW(&bodyClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Could not register the main form viewport.");
    app.body = CreateWindowExW(WS_EX_CONTROLPARENT, bodyClass.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, app.window, reinterpret_cast<HMENU>(IDC_MAIN_BODY), bodyClass.hInstance, nullptr);
    if (!app.body) throw std::runtime_error("Could not create the main form viewport.");
    auto add = [&](int id, const wchar_t* type, const wchar_t* title, DWORD style, DWORD ex = 0) {
        const auto control = CreateWindowExW(ex, type, title, WS_VISIBLE | WS_CHILD | style,
            0, 0, 0, 0, IsFooterControl(id) ? app.window : app.body, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
        if (!control) throw std::runtime_error("Could not create a window control.");
        SetWindowSubclass(control, FocusProcedure, 1, reinterpret_cast<DWORD_PTR>(app.window));
        return control;
    };
    const DWORD edit = WS_TABSTOP | ES_AUTOHSCROLL;
    const DWORD button = WS_TABSTOP | BS_PUSHBUTTON;
    add(IDC_PRESET_LABEL, L"STATIC", L"Pr&eset:", 0);
    auto preset = add(IDC_PRESET, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL);
    SendMessageW(preset, CB_LIMITTEXT, dice::MaxPresetName, 0);
    COMBOBOXINFO combo{sizeof(combo)};
    if (GetComboBoxInfo(preset, &combo))
        SetWindowSubclass(combo.hwndItem, FocusProcedure, 1, reinterpret_cast<DWORD_PTR>(app.window));
    add(IDC_PRESET_APPLY, L"BUTTON", L"&Use", button);
    add(IDC_PRESET_SAVE, L"BUTTON", L"Save preset", button);
    add(IDC_PRESET_DELETE, L"BUTTON", L"Delete preset", button);
    add(IDC_GROUP_LABEL, L"STATIC", L"Group:", 0);
    add(IDC_GROUP, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL);
    SendMessageW(Control(app, IDC_GROUP), CB_LIMITTEXT, dice::MaxPresetName, 0);
    add(IDC_IMPORT, L"BUTTON", L"Import presets", button);
    add(IDC_EXPORT_PRESETS, L"BUTTON", L"Export presets", button);
    add(IDC_QUICK_LABEL, L"STATIC", L"Quick dice:", 0);
    for (int i = 0; i < 7; ++i) {
        const auto title = L"d" + std::to_wstring(QuickSides[i]);
        add(IDC_QUICK_FIRST + i, L"BUTTON", title.c_str(), button);
    }
    add(IDC_QUICK_IMMEDIATE, L"BUTTON", L"Roll once on quick-die click", BS_AUTOCHECKBOX | WS_TABSTOP);
    SendMessageW(Control(app, IDC_QUICK_IMMEDIATE), BM_SETCHECK,
        app.settings.quickRollImmediately ? BST_CHECKED : BST_UNCHECKED, 0);
    add(IDC_DICE_LABEL, L"STATIC", L"&Dice:", 0);
    SendMessageW(add(IDC_DICE_EDIT, L"EDIT", app.settings.notation.c_str(), edit, WS_EX_CLIENTEDGE), EM_LIMITTEXT, 1024, 0);
    add(IDC_TIMES_LABEL, L"STATIC", L"&Times:", 0);
    SendMessageW(add(IDC_TIMES_EDIT, L"EDIT", std::to_wstring(app.settings.times).c_str(), edit | ES_NUMBER, WS_EX_CLIENTEDGE), EM_LIMITTEXT, 5, 0);
    add(IDC_MODE_GROUP, L"BUTTON", L"Mode", BS_GROUPBOX);
    add(IDC_RAD_NORMAL, L"BUTTON", L"N&ormal", BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP);
    add(IDC_RAD_ADV, L"BUTTON", L"&Advantage", BS_AUTORADIOBUTTON);
    add(IDC_RAD_DISADV, L"BUTTON", L"Disad&vantage", BS_AUTORADIOBUTTON);
    SetMode(app, app.settings.mode);
    add(IDC_RULES, L"STATIC", L"Roll whole group twice: Advantage keeps higher; Disadvantage lower.\r\n"
        L"Examples: 2d6+1d4+3; 4d6kh3; 4d6kl2. Pool counts kept dice meeting its threshold.", SS_NOPREFIX | WS_GROUP);
    add(IDC_NAME_LABEL, L"STATIC", L"&Name:", 0);
    SendMessageW(add(IDC_NAME_EDIT, L"EDIT", app.settings.name.c_str(), edit, WS_EX_CLIENTEDGE), EM_LIMITTEXT, 128, 0);
    add(IDC_TARGET, L"BUTTON", L"Target", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP);
    auto comparison = add(IDC_COMPARISON, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP);
    for (const auto* op : {L">=", L"<=", L"=", L">", L"<"}) SendMessageW(comparison, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(op));
    SendMessageW(comparison, CB_SETCURSEL, static_cast<int>(app.settings.target.comparison), 0);
    SendMessageW(Control(app, IDC_TARGET), BM_SETCHECK, app.settings.target.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(add(IDC_TARGET_VALUE, L"EDIT", std::to_wstring(app.settings.target.value).c_str(), edit, WS_EX_CLIENTEDGE), EM_LIMITTEXT, 20, 0);
    add(IDC_POOL, L"BUTTON", L"Pool: each die >=", BS_AUTOCHECKBOX | WS_TABSTOP);
    SendMessageW(Control(app, IDC_POOL), BM_SETCHECK, app.settings.poolEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(add(IDC_POOL_VALUE, L"EDIT", std::to_wstring(app.settings.successThreshold).c_str(), edit | ES_NUMBER, WS_EX_CLIENTEDGE), EM_LIMITTEXT, 10, 0);
    add(IDC_TIMESTAMP, L"BUTTON", L"Timestamp on new results", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP);
    SendMessageW(Control(app, IDC_TIMESTAMP), BM_SETCHECK, app.settings.showTimestamp ? BST_CHECKED : BST_UNCHECKED, 0);
    add(IDC_AUTOSAVE, L"BUTTON", L"Automatically append to CSV", BS_AUTOCHECKBOX | WS_TABSTOP);
    SendMessageW(Control(app, IDC_AUTOSAVE), BM_SETCHECK, app.settings.autoSave ? BST_CHECKED : BST_UNCHECKED, 0);
    add(IDC_FILE_LABEL, L"STATIC", L"CSV &path:", 0);
    auto file = add(IDC_FILE_EDIT, L"EDIT", app.settings.csvPath.c_str(), edit, WS_EX_CLIENTEDGE);
    SendMessageW(file, EM_LIMITTEXT, 32760, 0);
    add(IDC_BROWSE, L"BUTTON", L"&Browse...", button);
    add(IDC_ROLL, L"BUTTON", L"&Roll", WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON);
    add(IDC_CANCEL, L"BUTTON", L"Cancel", button);
    add(IDC_SAVE, L"BUTTON", L"E&xport pending", button);
    add(IDC_DISCARD, L"BUTTON", L"&History...", button);
    add(IDC_PROBABILITY, L"BUTTON", L"Probability / compare", button);
    add(IDC_OPEN, L"BUTTON", L"Open CSV", button);
    add(IDC_COPY, L"BUTTON", L"&Copy log", button);
    add(IDC_CLEAR, L"BUTTON", L"C&lear log", button);
    add(IDC_PROGRESS, PROGRESS_CLASSW, L"", PBS_SMOOTH);
    add(IDC_STATUS, L"EDIT", L"Ready. Batches are kept locally; export status is tracked separately.",
        WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY);
    add(IDC_OUTPUT_LABEL, L"STATIC", L"Recent results (complete records: History):", 0);
    add(IDC_OUTPUT, L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, WS_EX_CLIENTEDGE);
    SendMessageW(Control(app, IDC_OUTPUT), EM_LIMITTEXT, dice::LogBuffer::MaxCharacters, 0);
    add(IDC_RETENTION, L"STATIC", L"", SS_NOPREFIX);
    RefreshPresets(app);
    SetFont(app);
    Layout(app);
    RefreshLog(app);
    EnableActions(app);
    if (!SetTimer(app.window, PollTimer, 50, nullptr)) throw std::runtime_error("Could not start progress timer.");
}

std::filesystem::path OutputPath(App& app) {
    const auto path = dice::ResolvePath(Text(Control(app, IDC_FILE_EDIT)), app.base);
    SetWindowTextW(Control(app, IDC_FILE_EDIT), path.c_str());
    return path;
}

dice::Mode CurrentMode(const App& app) {
    return Checked(app, IDC_RAD_ADV) ? dice::Mode::Advantage
        : Checked(app, IDC_RAD_DISADV) ? dice::Mode::Disadvantage : dice::Mode::Normal;
}

std::int64_t ReadInteger(const std::wstring& input, const char* description) {
    const auto value = dice::Trim(input);
    std::size_t end = 0;
    try {
        const auto number = std::stoll(value, &end);
        if (end == value.size()) return number;
    } catch (const std::exception&) {}
    throw std::invalid_argument(description);
}
dice::Request ReadRules(const App& app) {
    dice::Request request;
    request.name = L"Probability";
    request.notation = dice::Trim(Text(Control(app, IDC_DICE_EDIT)));
    request.spec = dice::Parse(request.notation);
    request.times = 1;
    request.mode = CurrentMode(app);
    request.showTimestamp = Checked(app, IDC_TIMESTAMP);
    request.target.enabled = Checked(app, IDC_TARGET);
    request.target.comparison = static_cast<dice::Compare>(SendMessageW(Control(app, IDC_COMPARISON), CB_GETCURSEL, 0, 0));
    if (request.target.enabled)
        request.target.value = ReadInteger(Text(Control(app, IDC_TARGET_VALUE)), "Enter an integer target within the 64-bit range.");
    if (Checked(app, IDC_POOL)) {
        const auto threshold = ReadInteger(Text(Control(app, IDC_POOL_VALUE)), "Enter a positive integer pool threshold.");
        if (threshold < 1 || threshold > INT_MAX) throw std::invalid_argument("Pool threshold must be between 1 and 2147483647.");
        request.spec.successThreshold = static_cast<int>(threshold);
    }
    dice::Validate(request);
    return request;
}
dice::Request ReadRequest(const App& app, bool singleRoll) {
    auto request = ReadRules(app);
    request.name = dice::Trim(Text(Control(app, IDC_NAME_EDIT)));
    request.times = singleRoll ? 1 : dice::ParseTimes(Text(Control(app, IDC_TIMES_EDIT)));
    dice::Validate(request);
    return request;
}
void RestoreRequest(App& app, const dice::Request& request) {
    SetWindowTextW(Control(app, IDC_DICE_EDIT), request.notation.c_str());
    SetWindowTextW(Control(app, IDC_TIMES_EDIT), std::to_wstring(request.times).c_str());
    SetWindowTextW(Control(app, IDC_NAME_EDIT), request.name.c_str());
    SetMode(app, request.mode);
    SendMessageW(Control(app, IDC_TARGET), BM_SETCHECK, request.target.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(Control(app, IDC_COMPARISON), CB_SETCURSEL, static_cast<int>(request.target.comparison), 0);
    if (request.target.enabled) SetWindowTextW(Control(app, IDC_TARGET_VALUE), std::to_wstring(request.target.value).c_str());
    SendMessageW(Control(app, IDC_POOL), BM_SETCHECK, request.spec.successThreshold ? BST_CHECKED : BST_UNCHECKED, 0);
    if (request.spec.successThreshold) SetWindowTextW(Control(app, IDC_POOL_VALUE), std::to_wstring(request.spec.successThreshold).c_str());
}

void RememberWindow(App& app) {
    if (!IsWindow(app.window) || IsIconic(app.window)) return;
    app.settings.window.maximized = IsZoomed(app.window) != FALSE;
    if (IsZoomed(app.window)) return;
    RECT rect{};
    GetWindowRect(app.window, &rect);
    app.settings.window.valid = true;
    app.settings.window.x = rect.left;
    app.settings.window.y = rect.top;
    app.settings.window.width = MulDiv(rect.right - rect.left, 96, static_cast<int>(app.dpi));
    app.settings.window.height = MulDiv(rect.bottom - rect.top, 96, static_cast<int>(app.dpi));
}

dice::AppSettings CaptureSettings(App& app) {
    RememberWindow(app);
    auto settings = app.settings;
    // An unfinished/invalid edit must not corrupt a working saved configuration.
    try {
        const auto request = ReadRequest(app);
        settings.name = request.name;
        settings.notation = request.notation;
        settings.times = request.times;
        settings.mode = request.mode;
        settings.poolEnabled = request.spec.successThreshold != 0;
        if (request.spec.successThreshold) settings.successThreshold = request.spec.successThreshold;
        if (request.target.enabled) settings.target = request.target;
    } catch (const std::exception&) { /* Keep the last valid roll configuration. */ }
    // Even if another field is unfinished, switching a rule off is safe and
    // its valid draft should survive restarting the application.
    if (!Checked(app, IDC_TARGET)) {
        settings.target.enabled = false;
        settings.target.comparison = static_cast<dice::Compare>(SendMessageW(Control(app, IDC_COMPARISON), CB_GETCURSEL, 0, 0));
        try { settings.target.value = ReadInteger(Text(Control(app, IDC_TARGET_VALUE)), "Invalid target."); } catch (const std::exception&) {}
    }
    if (!Checked(app, IDC_POOL)) {
        settings.poolEnabled = false;
        try {
            const auto threshold = ReadInteger(Text(Control(app, IDC_POOL_VALUE)), "Invalid threshold.");
            if (threshold >= 1 && threshold <= INT_MAX) settings.successThreshold = static_cast<int>(threshold);
        } catch (const std::exception&) {}
    }
    const auto path = Text(Control(app, IDC_FILE_EDIT));
    try { settings.csvPath = path.empty() ? L"" : dice::ResolvePath(path, app.base).wstring(); }
    catch (const std::exception&) { /* Keep the last valid destination. */ }
    settings.autoSave = Checked(app, IDC_AUTOSAVE);
    settings.showTimestamp = Checked(app, IDC_TIMESTAMP);
    settings.quickRollImmediately = Checked(app, IDC_QUICK_IMMEDIATE);
    return settings;
}

void CommitSettings(App& app, dice::AppSettings settings) {
    if (!app.settingsPath.empty()) {
        if (app.settingsNeedBackup) {
            dice::BackupSettings(app.settingsPath);
            app.settingsNeedBackup = false;
        }
        dice::SaveSettings(app.settingsPath, settings);
    }
    app.settings = std::move(settings);
}

void RefreshPresets(App& app, const std::wstring& selected, bool rebuildGroups) {
    const auto combo = Control(app, IDC_PRESET);
    const auto group = dice::Trim(Text(Control(app, IDC_GROUP)));
    const auto groups = Control(app, IDC_GROUP);
    if (rebuildGroups) {
        SendMessageW(groups, CB_RESETCONTENT, 0, 0);
        SendMessageW(groups, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L""));
    }
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& preset : app.settings.presets) {
        if (rebuildGroups && SendMessageW(groups, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(preset.group.c_str())) == CB_ERR)
            SendMessageW(groups, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.group.c_str()));
        if (group.empty() || CompareStringOrdinal(group.c_str(), -1, preset.group.c_str(), -1, TRUE) == CSTR_EQUAL)
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.name.c_str()));
    }
    if (rebuildGroups) {
        const auto index = SendMessageW(groups, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(group.c_str()));
        if (index != CB_ERR) SendMessageW(groups, CB_SETCURSEL, index, 0);
        else SetWindowTextW(groups, group.c_str());
    }
    SetWindowTextW(combo, selected.c_str());
}

std::size_t FindPreset(const App& app, const std::wstring& name) {
    for (std::size_t i = 0; i < app.settings.presets.size(); ++i)
        if (CompareStringOrdinal(dice::Trim(app.settings.presets[i].name).c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL) return i;
    return app.settings.presets.size();
}

void ApplyPreset(App& app) {
    if (Busy(app)) return;
    const auto name = dice::Trim(Text(Control(app, IDC_PRESET)));
    const auto index = FindPreset(app, name);
    if (index == app.settings.presets.size()) throw std::invalid_argument("Choose a saved preset, then select Use.");
    const auto& preset = app.settings.presets[index];
    dice::Request request{preset.name, preset.notation, dice::Parse(preset.notation), preset.mode, preset.times};
    request.spec.successThreshold = preset.successThreshold;
    request.target = preset.target;
    RestoreRequest(app, request);
    const auto presetName = preset.name;
    const auto presetGroup = preset.group;
    SetWindowTextW(Control(app, IDC_GROUP), presetGroup.c_str());
    RefreshPresets(app, presetName);
    Status(app, L"Preset applied to the next roll: " + preset.name);
    SetFocus(Control(app, IDC_DICE_EDIT));
    EnsureFocusVisible(app);
}

void SavePreset(App& app) {
    if (Busy(app)) return;
    dice::Preset preset{dice::Trim(Text(Control(app, IDC_PRESET))),
        dice::Trim(Text(Control(app, IDC_DICE_EDIT))), dice::ParseTimes(Text(Control(app, IDC_TIMES_EDIT))), CurrentMode(app)};
    auto request = ReadRules(app);
    request.name = preset.name;
    request.times = preset.times;
    dice::Validate(request);
    preset.group = dice::Trim(Text(Control(app, IDC_GROUP)));
    preset.successThreshold = request.spec.successThreshold;
    preset.target = request.target;
    dice::ValidatePreset(preset);
    const auto index = FindPreset(app, preset.name);
    if (index < app.settings.presets.size() && app.confirm(app.window,
        L"Replace the saved preset with these dice, times and mode?", L"Replace preset",
        MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
    auto settings = CaptureSettings(app);
    if (index == settings.presets.size()) {
        if (settings.presets.size() == dice::MaxPresets) throw std::invalid_argument("At most 50 presets can be saved. Delete an unused preset first.");
        settings.presets.push_back(preset);
    } else settings.presets[index] = preset;
    CommitSettings(app, std::move(settings));
    RefreshPresets(app, preset.name);
    Status(app, L"Preset saved: " + preset.name);
}

void DeletePreset(App& app) {
    if (Busy(app)) return;
    const auto name = dice::Trim(Text(Control(app, IDC_PRESET)));
    const auto index = FindPreset(app, name);
    if (index == app.settings.presets.size()) throw std::invalid_argument("Choose a saved preset to delete.");
    if (app.confirm(app.window, (L"Delete preset \"" + name + L"\"?").c_str(), L"Delete preset",
        MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
    auto settings = CaptureSettings(app);
    settings.presets.erase(settings.presets.begin() + index);
    CommitSettings(app, std::move(settings));
    RefreshPresets(app);
    Status(app, L"Preset deleted. Generated results are unchanged.");
}

void QuickDie(App& app, int index) {
    if (Busy(app) || index < 0 || index >= 7) return;
    SetWindowTextW(Control(app, IDC_DICE_EDIT), (L"1d" + std::to_wstring(QuickSides[index])).c_str());
    if (Checked(app, IDC_QUICK_IMMEDIATE)) {
        StartRoll(app, true);
        return;
    }
    SetFocus(Control(app, IDC_DICE_EDIT));
    SendMessageW(Control(app, IDC_DICE_EDIT), EM_SETSEL, 0, -1);
    Status(app, L"Quick die selected. Times and mode are unchanged; select Roll when ready.");
    EnsureFocusVisible(app);
}

void StartRoll(App& app, bool singleRoll) {
    if (Busy(app)) return;
    auto request = ReadRequest(app, singleRoll);
    std::optional<std::filesystem::path> path;
    if (Checked(app, IDC_AUTOSAVE)) path = OutputPath(app);
    app.settings = CaptureSettings(app);
    app.requested = request.times;
    app.saveOnly = false;
    app.job.StartRoll(std::move(request), path, app.historyRoot);
    SendMessageW(Control(app, IDC_PROGRESS), PBM_SETRANGE32, 0, app.requested);
    SendMessageW(Control(app, IDC_PROGRESS), PBM_SETPOS, 0, 0);
    Status(app, L"Rolling...");
    EnableActions(app);
}

void SavePending(App& app) {
    if (Busy(app) || !HasPending(app)) return;
    const auto path = OutputPath(app);
    if (app.historyRoot.empty()) app.job.StartSave(app.pending, path);
    else {
        std::vector<dice::HistorySummary> entries;
        for (const auto& entry : app.history) if (!entry.saved && entry.resultCount) entries.push_back(entry);
        app.job.StartExportIds(std::move(entries), path, app.historyRoot, app.dirtyHistory);
    }
    app.saveOnly = true;
    Status(app, L"Saving retained results...");
    EnableActions(app);
}

template<class T> void MergeById(std::vector<T>& entries, T entry) {
    auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.id == entry.id; });
    if (found == entries.end()) entries.push_back(std::move(entry));
    else *found = std::move(entry);
}
void CollectOutcome(App& app, dice::Outcome& outcome) {
    for (const auto& id : outcome.persistedIds) {
        app.dirtyHistory.erase(std::remove_if(app.dirtyHistory.begin(), app.dirtyHistory.end(),
            [&](const auto& e) { return e.id == id; }), app.dirtyHistory.end());
        app.pendingMarks.erase(std::remove_if(app.pendingMarks.begin(), app.pendingMarks.end(),
            [&](const auto& e) { return e.id == id; }), app.pendingMarks.end());
    }
    for (auto& entry : outcome.summaries) if (!entry.archived) MergeById(app.history, entry);
    for (auto& entry : outcome.dirtyEntries) {
        auto summary = dice::SummaryOf(entry); summary.localPersisted = false;
        MergeById(app.history, std::move(summary));
        MergeById(app.dirtyHistory, std::move(entry));
    }
    for (auto& mark : outcome.pendingMarks) {
        if (!mark.archived) MergeById(app.history, mark);
        MergeById(app.pendingMarks, std::move(mark));
    }
}
void StartHistoryScan(App& app) {
    app.cancelIndex = false;
    app.historyScan = std::async(std::launch::async, [&app] {
        return dice::ScanHistory(app.historyRoot, false, [&app] { return app.cancelIndex.load(); });
    });
    app.historyLoading = true;
    EnableActions(app);
    Status(app, L"Loading the history index...");
}
void Poll(App& app) {
    if (app.historyLoading) {
        if (app.historyScan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        app.historyLoading = false;
        try {
            auto index = app.historyScan.get();
            app.history = std::move(index.entries);
            for (const auto& warning : index.warnings) app.log.Add(warning + L"\r\n");
            RefreshLog(app);
            Status(app, !index.warnings.empty() ? L"History index loaded with warnings. Original files were kept; see the log."
                : HasPending(app) ? L"Recovered unexported batches. Open History to inspect or export; you can roll again."
                : L"Ready. History details load when selected.");
        } catch (const std::exception& error) {
            if (!app.closing) { app.log.Add(dice::Wide(error.what()) + L"\r\n"); RefreshLog(app); Status(app, L"History scan failed; open History and Reload to retry."); }
        }
        if (app.closing) { FinishClose(app); return; }
        EnableActions(app);
    }
    if (!Busy(app)) return;
    const auto phase = app.job.State();
    if (phase != dice::Phase::Finished) {
        if (phase == dice::Phase::Rolling) {
            const auto count = app.job.Completed();
            SendMessageW(Control(app, IDC_PROGRESS), PBM_SETPOS, count, 0);
            Status(app, (app.closing ? L"Cancelling and closing... " : L"Rolling: ") +
                std::to_wstring(count) + L" / " + std::to_wstring(app.requested));
        } else Status(app, app.closing ? L"Keeping recovery data before closing..." : L"Saving CSV... You can cancel before the file is committed.");
        return;
    }
    auto outcome = app.job.Take();
    std::size_t count = outcome.batch ? outcome.batch->results.size() : 0;
    if (!outcome.batch) for (const auto& entry : outcome.summaries) count += entry.resultCount;
    // Retain results before display allocation, so display errors cannot lose the batch.
    if (app.historyRoot.empty()) app.pending = !outcome.saved && count ? outcome.batch : nullptr;
    CollectOutcome(app, outcome);
    if (app.flushingClose) app.flushingClose = false;
    if (app.closing) { FinishClose(app); return; }
    EnableActions(app);
    if (!app.saveOnly && outcome.batch) {
        app.log.Add(L"--- " + outcome.batch->request.name + L": " + std::to_wstring(count) + L" completed roll(s) ---\r\n");
        for (std::size_t i = 0; i < count; ++i)
            app.log.Add(dice::FormatResult(outcome.batch->request, outcome.batch->results[i], i + 1));
        SendMessageW(Control(app, IDC_PROGRESS), PBM_SETPOS, count, 0);
    }
    std::wstring message;
    if (outcome.saved) message = L"Saved " + std::to_wstring(count) + L" roll(s) to: " + outcome.path.wstring();
    else if (outcome.csvCommitted) message = L"CSV export completed. Its local status still needs to be saved; closing will retry. Destination: " + outcome.path.wstring();
    else {
        message = outcome.cancelled ? L"Cancelled. " : outcome.error.empty() ? L"Completed. " : L"Operation failed. ";
        message += std::to_wstring(count) + L" roll(s) retained; ";
        message += count ? L"use History to inspect, compare or export. You can roll again." : L"no new results.";
    }
    app.log.Add(message + L"\r\n");
    RefreshLog(app);
    Status(app, message);
    if (!outcome.error.empty())
        MessageBoxW(app.window, (outcome.error + L"\n\n" + message).c_str(), L"Dice Roller", MB_OK | MB_ICONERROR);
}

void History(App& app) {
    if (Busy(app)) return;
    std::filesystem::path exportPath;
    auto selection = ui::ShowHistory(app.window, app.historyRoot, app.history, app.dirtyHistory,
        [&] { exportPath = OutputPath(app); }, app.pendingMarks);
    if (selection.indexChanged) {
        app.history.clear();
        for (auto& entry : selection.index) if (!entry.archived) app.history.push_back(std::move(entry));
        for (const auto& entry : app.dirtyHistory) {
            auto summary = dice::SummaryOf(entry); summary.localPersisted = false;
            MergeById(app.history, std::move(summary));
        }
        for (const auto& mark : app.pendingMarks) if (!mark.archived) MergeById(app.history, mark);
        EnableActions(app);
    }
    if (selection.action == ui::HistorySelection::Action::Reuse && selection.request) {
        RestoreRequest(app, *selection.request);
        Status(app, L"Historical parameters loaded. Select Roll to generate NEW random results.");
        SetFocus(Control(app, IDC_DICE_EDIT));
        EnsureFocusVisible(app);
    } else if (selection.action == ui::HistorySelection::Action::Export && !selection.selected.empty()) {
        std::vector<dice::HistoryEntry> dirty;
        for (const auto& entry : app.dirtyHistory)
            if (std::any_of(selection.selected.begin(), selection.selected.end(), [&](const auto& e) { return e.id == entry.id; })) dirty.push_back(entry);
        app.job.StartExportIds(std::move(selection.selected), exportPath, app.historyRoot, std::move(dirty));
        app.saveOnly = true;
        EnableActions(app);
        Status(app, L"Exporting selected batches...");
    }
}
void ExchangePresets(App& app, bool importing) {
    if (Busy(app)) return;
    wchar_t filename[32768] = L"presets.drpresets";
    OPENFILENAMEW picker{sizeof(picker)};
    picker.hwndOwner = app.window;
    picker.lpstrFile = filename;
    picker.nMaxFile = static_cast<DWORD>(std::size(filename));
    picker.lpstrFilter = L"Dice Roller presets (*.drpresets)\0*.drpresets\0";
    picker.lpstrDefExt = L"drpresets";
    picker.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_EXPLORER |
        (importing ? OFN_FILEMUSTEXIST : OFN_OVERWRITEPROMPT);
    if (!(importing ? GetOpenFileNameW(&picker) : GetSaveFileNameW(&picker))) {
        if (CommDlgExtendedError()) throw std::runtime_error("The preset file picker failed.");
        return;
    }
    if (importing) {
        const auto incoming = dice::ReadPresets(filename);
        const auto preview = dice::PreviewImport(app.settings.presets, incoming);
        const auto strategy = ui::ChooseImportStrategy(app.window, preview);
        if (!strategy) return;
        auto settings = CaptureSettings(app);
        settings.presets = dice::MergePresets(settings.presets, incoming, *strategy);
        CommitSettings(app, std::move(settings));
        SetWindowTextW(Control(app, IDC_GROUP), L"");
        RefreshPresets(app);
        Status(app, L"Imported presets. Groups and roll rules were retained.");
    } else {
        dice::ExportPresets(filename, app.settings.presets);
        Status(app, L"All presets exported, including groups and roll rules.");
    }
}

struct BrowseStart { std::wstring directory, filename; };
BrowseStart PrepareBrowse(const std::wstring& input, const std::filesystem::path& base) {
    auto candidate = base / L"dice_results_v3.csv";
    try { candidate = dice::ResolvePath(input, base); }
    catch (const std::exception&) { /* Browsing is also how an invalid path is corrected. */ }
    auto directory = candidate.parent_path();
    std::error_code error;
    while (!directory.empty() && !std::filesystem::is_directory(directory, error)) {
        const auto parent = directory.parent_path();
        if (parent == directory) { directory.clear(); break; }
        directory = parent;
        error.clear();
    }
    return {directory.wstring(), candidate.filename().wstring()};
}
void Browse(App& app) {
    if (Busy(app)) return;
    std::vector<wchar_t> buffer(32768, L'\0');
    const auto start = PrepareBrowse(Text(Control(app, IDC_FILE_EDIT)), app.base);
    const auto filename = start.filename.size() <= 255 &&
        start.filename.find_first_of(L"<>:\"/\\|?*") == std::wstring::npos
        ? start.filename : L"dice_results_v3.csv";
    std::copy(filename.begin(), filename.end(), buffer.begin());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app.window;
    ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0";
    ofn.lpstrFile = buffer.data();
    ofn.lpstrInitialDir = start.directory.empty() ? nullptr : start.directory.c_str();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrDefExt = L"csv";
    ofn.lpstrTitle = L"Choose CSV to append to (existing records are kept)";
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (app.filePicker(&ofn)) {
        const auto path = dice::ResolvePath(buffer.data(), app.base);
        SetWindowTextW(Control(app, IDC_FILE_EDIT), path.c_str());
    }
    else if (const auto error = CommDlgExtendedError())
        throw std::runtime_error("File picker failed (error " + std::to_string(error) + ").");
}

void OpenCsv(App& app) {
    if (Busy(app)) return;
    const auto path = OutputPath(app);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) throw std::runtime_error("This CSV file does not exist or cannot be read.");
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(app.window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        throw std::runtime_error("Could not open CSV. Check the file association and file permissions.");
}

void CopyLog(App& app) {
    const auto text = app.log.Text();
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) throw std::runtime_error("Could not allocate clipboard memory.");
    auto data = GlobalLock(memory);
    if (!data) { GlobalFree(memory); throw std::runtime_error("Could not access clipboard memory."); }
    std::memcpy(data, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(app.window)) { GlobalFree(memory); throw std::runtime_error("Clipboard is busy. Try again."); }
    const bool ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!ok) { GlobalFree(memory); throw std::runtime_error("Could not copy the log."); }
}

INT_PTR CALLBACK About(HWND dialog, UINT msg, WPARAM wParam, LPARAM) {
    if (msg == WM_INITDIALOG) return TRUE;
    if (msg == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)) {
        EndDialog(dialog, LOWORD(wParam));
        return TRUE;
    }
    return FALSE;
}
bool FinishClose(App& app) {
    if (!app.historyRoot.empty() && (!app.dirtyHistory.empty() || !app.pendingMarks.empty())) {
        if (!app.closeFlushAttempted) {
            try { app.job.StartFlushDirty(app.dirtyHistory, app.historyRoot, app.pendingMarks); }
            catch (...) { app.closing = false; throw; }
            app.closeFlushAttempted = true;
            app.flushingClose = true;
            app.saveOnly = true;
            EnableActions(app);
            Status(app, L"Keeping recovery data before closing...");
            return false;
        }
        {
            const auto message = app.dirtyHistory.empty()
                ? L"CSV export succeeded, but its local status could not be saved.\n\nClose anyway? On reopening, these batches may appear unexported. Exporting to the same CSV will not duplicate them."
                : L"Some results could not be kept for recovery.\n\nClose anyway? Results not written to disk may be lost.";
            if (app.confirm(app.window, message, L"Local history", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
                app.closing = false; app.closeFlushAttempted = false; EnableActions(app); return false;
            }
        }
    }
    try { CommitSettings(app, CaptureSettings(app)); }
    catch (const std::exception& error) {
        std::wstring detail = L"The configuration file could not be written.";
        try { detail = dice::Wide(error.what()); } catch (...) {}
        const auto message = L"Settings could not be saved.\n" + detail +
            L"\n\nClose anyway and keep the previously saved settings?";
        if (app.confirm(app.window, message.c_str(), L"Save settings", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
            app.closing = false;
            app.closeFlushAttempted = false;
            EnableActions(app);
            return false;
        }
    }
    DestroyWindow(app.window);
    return true;
}
void Close(App& app) {
    if (app.closing || app.closePrompt) return;
    struct PromptGuard {
        bool& active;
        explicit PromptGuard(bool& flag) : active(flag) { active = true; }
        ~PromptGuard() { active = false; }
    } prompt(app.closePrompt);
    if (app.historyLoading) {
        app.closing = true;
        app.cancelIndex = true;
        EnableActions(app);
        return;
    }
    if (Busy(app)) {
        if (app.confirm(app.window, L"Cancel the operation and close? Completed rolls will be kept in local history.",
            L"Close Dice Roller", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
        // The modal dialog pumps WM_TIMER: Poll may have completed/collected the
        // worker while the question was open. Never reuse the pre-dialog state.
        app.closing = true;
        if (Busy(app)) { app.job.Cancel(); EnableActions(app); }
        else FinishClose(app);
        return;
    }
    if (app.historyRoot.empty() && app.pending && app.confirm(app.window, L"Close and discard the unsaved batch?", L"Close Dice Roller",
        MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
    app.closing = true;
    FinishClose(app);
}

LRESULT CALLBACK WindowProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    try {
        if (msg == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            app->window = window;
            app->dpi = GetDpiForWindow(window);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(window, msg, wParam, lParam);
        if (msg == ui::ActivationMessage()) {
            if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
            HWND active = GetLastActivePopup(window);
            if (!IsWindowVisible(active)) active = window;
            SetForegroundWindow(active);
            return 0;
        }
        switch (msg) {
        case WM_CREATE: Setup(*app); return 0;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED && !app->layingOut) {
                // Resizing preserves the reading position. Keyboard focus changes
                // scroll explicitly; an old off-screen edit must not pull us back.
                Layout(*app); RememberWindow(*app);
            }
            return 0;
        case WM_MOVE: RememberWindow(*app); return 0;
        case FocusChanged: EnsureFocusVisible(*app); return 0;
        case WM_VSCROLL: Scroll(*app, SB_VERT, LOWORD(wParam)); return 0;
        case WM_HSCROLL: Scroll(*app, SB_HORZ, LOWORD(wParam)); return 0;
        case WM_MOUSEWHEEL:
            app->wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
            while (app->wheelRemainder >= WHEEL_DELTA) { Scroll(*app, SB_VERT, SB_LINEUP); app->wheelRemainder -= WHEEL_DELTA; }
            while (app->wheelRemainder <= -WHEEL_DELTA) { Scroll(*app, SB_VERT, SB_LINEDOWN); app->wheelRemainder += WHEEL_DELTA; }
            return 0;
        case WM_DPICHANGED: {
            app->dpi = HIWORD(wParam);
            SetFont(*app);
            const auto requested = *reinterpret_cast<RECT*>(lParam);
            const auto rect = ui::FitToWorkArea(requested, ui::WorkAreaFor(requested));
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left,
                rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(*app);
            EnsureFocusVisible(*app);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            RECT rect{};
            GetWindowRect(window, &rect);
            auto limits = reinterpret_cast<MINMAXINFO*>(lParam);
            const auto size = ui::MinimumWindowSize(app->dpi, ui::WorkAreaFor(rect));
            limits->ptMinTrackSize = {size.cx, size.cy};
            return 0;
        }
        case DM_GETDEFID: return MAKELONG(app->defaultButton, DC_HASDEFID);
        case DM_SETDEFID:
            if (Control(*app, app->defaultButton)) SendMessageW(Control(*app, app->defaultButton), BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
            app->defaultButton = LOWORD(wParam);
            if (Control(*app, app->defaultButton)) SendMessageW(Control(*app, app->defaultButton), BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
            return TRUE;
        case WM_TIMER: if (wParam == PollTimer) Poll(*app); return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDC_GROUP && HIWORD(wParam) == CBN_SELCHANGE) {
                // The combo edit is updated after CBN_SELCHANGE returns.
                const int selected = static_cast<int>(SendMessageW(Control(*app, IDC_GROUP), CB_GETCURSEL, 0, 0));
                wchar_t group[128]{};
                if (selected != CB_ERR) SendMessageW(Control(*app, IDC_GROUP), CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(group));
                SetWindowTextW(Control(*app, IDC_GROUP), group);
                RefreshPresets(*app, {}, false); return 0;
            }
            if (id == IDC_GROUP && HIWORD(wParam) == CBN_KILLFOCUS) {
                RefreshPresets(*app, Text(Control(*app, IDC_PRESET)), false); return 0;
            }
            if (id == IDC_OUTPUT && (HIWORD(wParam) == EN_MAXTEXT || HIWORD(wParam) == EN_ERRSPACE)) {
                app->logFailed = true;
                return 0;
            }
            if (lParam && HIWORD(wParam) != BN_CLICKED) return 0;
            if (id >= IDC_QUICK_FIRST && id <= IDC_QUICK_LAST) { QuickDie(*app, id - IDC_QUICK_FIRST); return 0; }
            switch (id) {
            case IDOK: case IDC_ROLL: StartRoll(*app); break;
            case IDCANCEL: case IDC_CANCEL: if (Busy(*app)) app->job.Cancel(); break;
            case IDC_SAVE: SavePending(*app); break;
            case IDC_PRESET_APPLY: ApplyPreset(*app); break;
            case IDC_PRESET_SAVE: SavePreset(*app); break;
            case IDC_PRESET_DELETE: DeletePreset(*app); break;
            case IDC_DISCARD: History(*app); break;
            case IDC_PROBABILITY: if (!Busy(*app)) ui::ShowProbability(window, ReadRules(*app)); break;
            case IDC_IMPORT: ExchangePresets(*app, true); break;
            case IDC_EXPORT_PRESETS: ExchangePresets(*app, false); break;
            case IDC_BROWSE: Browse(*app); break;
            case IDC_OPEN: OpenCsv(*app); break;
            case IDC_COPY: CopyLog(*app); break;
            case IDC_CLEAR: app->log.Clear(); RefreshLog(*app); break;
            case IDM_ABOUT: DialogBoxW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUTBOX), window, About); break;
            case IDM_EXIT: Close(*app); break;
            }
            return 0;
        }
        case WM_CLOSE: Close(*app); return 0;
        case WM_DESTROY: KillTimer(window, PollTimer); PostQuitMessage(0); return 0;
        case WM_NCDESTROY: SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
        }
    } catch (const std::exception& error) {
        Error(window, error);
        if (msg == WM_CREATE) return -1;
    } catch (...) {
        MessageBoxW(window, L"Unexpected application error.", L"Dice Roller", MB_OK | MB_ICONERROR);
        if (msg == WM_CREATE) return -1;
    }
    return DefWindowProcW(window, msg, wParam, lParam);
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    try {
        ui::SingleInstance instanceGuard;
        if (!instanceGuard.IsPrimary()) {
            if (!ui::ActivateExistingInstance()) MessageBoxW(nullptr,
                L"Dice Roller is already running for this Windows user. Check your other desktop or session.",
                L"Dice Roller", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
        if (!InitCommonControlsEx(&controls)) throw std::runtime_error("Could not initialize Windows controls.");
        App app;
        app.base = dice::DefaultDirectory();
        app.settingsPath = dice::DefaultSettingsPath();
        app.historyRoot = dice::DefaultHistoryDirectory();
        const auto loaded = dice::LoadSettings(app.settingsPath, dice::DefaultSettings());
        app.settings = loaded.settings;
        app.settingsNeedBackup = !loaded.warning.empty();
        // Preserve legacy files; the new export format needs batch IDs and rule details.
        if (app.settings.csvPath == (app.base / L"dice_results_v2.csv").wstring())
            app.settings.csvPath = (app.base / L"dice_results_v3.csv").wstring();
        const auto placement = app.settings.window;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance;
        wc.lpfnWndProc = WindowProc;
        wc.lpszClassName = L"DiceGUIClass";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_DICEROLLER));
        wc.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_SMALL));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszMenuName = MAKEINTRESOURCEW(IDC_DICEROLLER);
        if (!RegisterClassExW(&wc)) throw std::runtime_error("Could not register the main window.");
        const UINT dpi = GetDpiForSystem();
        const int x = placement.valid ? placement.x : 40;
        const int y = placement.valid ? placement.y : 40;
        RECT rect{x, y, x + MulDiv(placement.width, static_cast<int>(dpi), 96),
            y + MulDiv(placement.height, static_cast<int>(dpi), 96)};
        rect = ui::FitToWorkArea(rect, ui::WorkAreaFor(rect));
        const auto window = CreateWindowExW(0, wc.lpszClassName, L"Dice Roller", MainStyle,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, instance, &app);
        if (!window) return 1;
        // Once created, GetDpiForWindow reflects the destination monitor, including
        // a restored secondary monitor with a different scale from the primary.
        rect.right = rect.left + Px(app, placement.width);
        rect.bottom = rect.top + Px(app, placement.height);
        rect = ui::FitToWorkArea(rect, ui::WorkAreaFor(rect));
        SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left,
            rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        const auto accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDC_DICEROLLER));
        ShowWindow(window, placement.maximized && show != SW_SHOWMINIMIZED ? SW_SHOWMAXIMIZED : show);
        UpdateWindow(window);
        if (!loaded.warning.empty()) Status(app, loaded.warning);
        if (!loaded.warning.empty()) { app.log.Add(loaded.warning + L"\r\n"); RefreshLog(app); }
        StartHistoryScan(app);
        MSG msg{};
        int status = 0;
        while ((status = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            const auto previousFocus = GetFocus();
            if (!TranslateAcceleratorW(window, accelerators, &msg) && !IsDialogMessageW(window, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (IsWindow(window) && GetFocus() != previousFocus) EnsureFocusVisible(app);
        }
        if (status == -1) throw std::runtime_error("The Windows message loop failed.");
        return static_cast<int>(msg.wParam);
    } catch (const std::exception& error) { Error(nullptr, error); return 1; }
}
