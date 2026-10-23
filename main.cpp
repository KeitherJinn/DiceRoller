#include "framework.h"
#include "Resource.h"
#include "RollJob.h"
#include "AppSettings.h"
#include "WindowLayout.h"
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
// Group-box interiors are transparent. Let the parent erase their old contents
// before repainting children after a scroll, rather than clipping out the whole box.
constexpr DWORD MainStyle = WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL;
constexpr int QuickSides[] = {4, 6, 8, 10, 12, 20, 100};
struct App {
    HWND window = nullptr;
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
    int scrollX = 0, scrollY = 0;
    int contentWidth = 0, contentHeight = 0;
    int wheelRemainder = 0;
    std::filesystem::path base;
    std::filesystem::path settingsPath;
    dice::AppSettings settings;
    std::function<int(HWND, LPCWSTR, LPCWSTR, UINT)> confirm = MessageBoxW;
    std::function<BOOL(OPENFILENAMEW*)> filePicker = GetSaveFileNameW;
    std::shared_ptr<const dice::Batch> pending;
    dice::LogBuffer log;
    dice::RollJob job;
    ~App() { if (font) DeleteObject(font); }
};
HWND Control(const App& app, int id) { return GetDlgItem(app.window, id); }
bool Checked(const App& app, int id) { return SendMessageW(Control(app, id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
bool Busy(const App& app) { return app.job.State() != dice::Phase::Idle; }
int Px(const App& app, int value) { return MulDiv(value, static_cast<int>(app.dpi), 96); }
void Layout(App& app);
void EnsureFocusVisible(App& app);
bool FinishClose(App& app);
void RefreshPresets(App& app, const std::wstring& selected = {});

std::wstring Text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(copied);
    return text;
}
void Status(App& app, const std::wstring& text) { SetWindowTextW(Control(app, IDC_STATUS), text.c_str()); }
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
                   IDC_PRESET_SAVE, IDC_PRESET_DELETE})
        EnableWindow(Control(app, id), !busy);
    for (int id = IDC_QUICK_FIRST; id <= IDC_QUICK_LAST; ++id) EnableWindow(Control(app, id), !busy);
    EnableWindow(Control(app, IDC_ROLL), !busy && !app.pending);
    EnableWindow(Control(app, IDC_CANCEL), busy && !app.closing);
    EnableWindow(Control(app, IDC_SAVE), !busy && static_cast<bool>(app.pending));
    EnableWindow(Control(app, IDC_DISCARD), !busy && static_cast<bool>(app.pending));
    const int focus = busy ? IDC_CANCEL : app.pending ? IDC_SAVE : IDC_ROLL;
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
          + L". CSV saves the full batch.";
    SetWindowTextW(Control(app, IDC_RETENTION), label.c_str());
}

void Layout(App& app) {
    if (app.layingOut) return;
    app.layingOut = true;
    RECT area{};
    GetClientRect(app.window, &area);
    // Show/hide scrollbars before measuring the final viewport. Keep content in
    // physical pixels so scrollbar positions and child rectangles agree at any DPI.
    for (int pass = 0; pass < 3; ++pass) {
        const int width = (std::max)(560, MulDiv(area.right, 96, static_cast<int>(app.dpi)));
        app.contentWidth = (std::max)(static_cast<int>(area.right), Px(app, width));
        app.contentHeight = (std::max)(static_cast<int>(area.bottom), Px(app, width < 800 ? 720 : 684));
        app.scrollX = (std::max)(0, (std::min)(app.scrollX, app.contentWidth - static_cast<int>(area.right)));
        app.scrollY = (std::max)(0, (std::min)(app.scrollY, app.contentHeight - static_cast<int>(area.bottom)));
        SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS, 0,
            app.contentWidth - 1, static_cast<UINT>(area.right), app.scrollX, 0};
        SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS, 0,
            app.contentHeight - 1, static_cast<UINT>(area.bottom), app.scrollY, 0};
        SetScrollInfo(app.window, SB_HORZ, &horizontal, TRUE);
        SetScrollInfo(app.window, SB_VERT, &vertical, TRUE);
        GetClientRect(app.window, &area);
    }
    const int width = MulDiv(app.contentWidth, 96, static_cast<int>(app.dpi));
    const int height = MulDiv(app.contentHeight, 96, static_cast<int>(app.dpi));
    const bool narrow = width < 800;
    auto move = [&](int id, int x, int y, int w, int h) {
        MoveWindow(Control(app, id), Px(app, x) - app.scrollX, Px(app, y) - app.scrollY,
            Px(app, (std::max)(1, w)), Px(app, (std::max)(1, h)), FALSE);
    };
    move(IDC_PRESET_LABEL, 16, 20, 80, 24);
    move(IDC_PRESET, 100, 16, width - 412, 240);
    move(IDC_PRESET_APPLY, width - 304, 16, 72, 26);
    move(IDC_PRESET_SAVE, width - 224, 16, 96, 26);
    move(IDC_PRESET_DELETE, width - 120, 16, 104, 26);
    move(IDC_QUICK_LABEL, 16, 56, 80, 24);
    const int quickWidth = (std::min)(64, (width - 116 - 6 * 8) / 7);
    for (int i = 0; i < 7; ++i) move(IDC_QUICK_FIRST + i, 100 + i * (quickWidth + 8), 52, quickWidth, 26);
    move(IDC_DICE_LABEL, 16, 92, 80, 24);
    move(IDC_DICE_EDIT, 100, 88, width - 330, 26);
    move(IDC_TIMES_LABEL, width - 208, 92, 70, 24);
    move(IDC_TIMES_EDIT, width - 128, 88, 112, 26);
    move(IDC_MODE_GROUP, 16, 124, width - 32, 56);
    move(IDC_RAD_NORMAL, 32, 144, 120, 24);
    move(IDC_RAD_ADV, 170, 144, 150, 24);
    move(IDC_RAD_DISADV, 336, 144, 180, 24);
    move(IDC_RULES, 16, 188, width - 32, 40);
    move(IDC_NAME_LABEL, 16, 240, 80, 24);
    move(IDC_NAME_EDIT, 100, 236, width - 116, 26);
    move(IDC_TIMESTAMP, 16, 272, 200, 24);
    move(IDC_AUTOSAVE, 232, 272, width - 248, 24);
    move(IDC_FILE_LABEL, 16, 312, 80, 24);
    move(IDC_FILE_EDIT, 100, 308, width - 224, 26);
    move(IDC_BROWSE, width - 108, 308, 92, 26);
    move(IDC_ROLL, 16, 348, 88, 28);
    move(IDC_CANCEL, 112, 348, 88, 28);
    move(IDC_SAVE, 208, 348, 104, 28);
    move(IDC_DISCARD, 320, 348, 104, 28);
    const int extra = narrow ? 36 : 0;
    move(IDC_OPEN, width - 308, 348 + extra, 92, 28);
    move(IDC_COPY, width - 208, 348 + extra, 92, 28);
    move(IDC_CLEAR, width - 108, 348 + extra, 92, 28);
    move(IDC_PROGRESS, 16, 386 + extra, width - 32, 16);
    move(IDC_STATUS, 16, 412 + extra, width - 32, 40);
    move(IDC_OUTPUT_LABEL, 16, 458 + extra, width - 32, 22);
    move(IDC_OUTPUT, 16, 484 + extra, width - 32, height - 534 - extra);
    move(IDC_RETENTION, 16, height - 42, width - 32, 36);
    app.layingOut = false;
    // Group boxes and radio buttons are siblings. Repaint the complete final
    // arrangement once; immediate per-control paints leave stale overlaps when scrolling.
    RedrawWindow(app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void EnsureFocusVisible(App& app) {
    const auto focus = GetFocus();
    if (!focus || !IsChild(app.window, focus)) return;
    RECT bounds{}, client{};
    GetWindowRect(focus, &bounds);
    MapWindowPoints(nullptr, app.window, reinterpret_cast<POINT*>(&bounds), 2);
    GetClientRect(app.window, &client);
    const int oldX = app.scrollX, oldY = app.scrollY;
    if (bounds.top < 0) app.scrollY += bounds.top;
    else if (bounds.bottom > client.bottom) app.scrollY += (std::min)(bounds.top, bounds.bottom - client.bottom);
    if (bounds.left < 0) app.scrollX += bounds.left;
    else if (bounds.right > client.right) app.scrollX += (std::min)(bounds.left, bounds.right - client.right);
    if (oldX != app.scrollX || oldY != app.scrollY) Layout(app);
}

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
    return DefSubclassProc(window, message, wParam, lParam);
}

void SetFont(App& app) {
    const auto font = CreateFontW(-MulDiv(10, static_cast<int>(app.dpi), 72), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!font) throw std::runtime_error("Could not create the interface font.");
    for (HWND child = GetWindow(app.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (app.font) DeleteObject(app.font);
    app.font = font;
}

void Setup(App& app) {
    auto add = [&](int id, const wchar_t* type, const wchar_t* title, DWORD style, DWORD ex = 0) {
        const auto control = CreateWindowExW(ex, type, title, WS_VISIBLE | WS_CHILD | style,
            0, 0, 0, 0, app.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
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
    add(IDC_QUICK_LABEL, L"STATIC", L"Quick dice:", 0);
    for (int i = 0; i < 7; ++i) {
        const auto title = L"d" + std::to_wstring(QuickSides[i]);
        add(IDC_QUICK_FIRST + i, L"BUTTON", title.c_str(), button);
    }
    add(IDC_DICE_LABEL, L"STATIC", L"&Dice:", 0);
    SendMessageW(add(IDC_DICE_EDIT, L"EDIT", app.settings.notation.c_str(), edit, WS_EX_CLIENTEDGE), EM_LIMITTEXT, 128, 0);
    add(IDC_TIMES_LABEL, L"STATIC", L"&Times:", 0);
    SendMessageW(add(IDC_TIMES_EDIT, L"EDIT", std::to_wstring(app.settings.times).c_str(), edit | ES_NUMBER, WS_EX_CLIENTEDGE), EM_LIMITTEXT, 5, 0);
    add(IDC_MODE_GROUP, L"BUTTON", L"Mode", BS_GROUPBOX);
    add(IDC_RAD_NORMAL, L"BUTTON", L"N&ormal", BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP);
    add(IDC_RAD_ADV, L"BUTTON", L"&Advantage", BS_AUTORADIOBUTTON);
    add(IDC_RAD_DISADV, L"BUTTON", L"Disad&vantage", BS_AUTORADIOBUTTON);
    CheckRadioButton(app.window, IDC_RAD_NORMAL, IDC_RAD_DISADV, IDC_RAD_NORMAL + static_cast<int>(app.settings.mode));
    add(IDC_RULES, L"STATIC", L"Roll whole group twice: Advantage keeps higher; Disadvantage lower.\r\n"
        L"Limits: 1,000 dice/group; 10,000 rolls; 1,000,000 dice/batch.", SS_NOPREFIX | WS_GROUP);
    add(IDC_NAME_LABEL, L"STATIC", L"&Name:", 0);
    SendMessageW(add(IDC_NAME_EDIT, L"EDIT", app.settings.name.c_str(), edit, WS_EX_CLIENTEDGE), EM_LIMITTEXT, 128, 0);
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
    add(IDC_SAVE, L"BUTTON", L"&Save batch", button);
    add(IDC_DISCARD, L"BUTTON", L"Discard batch", button);
    add(IDC_OPEN, L"BUTTON", L"Open CSV", button);
    add(IDC_COPY, L"BUTTON", L"&Copy log", button);
    add(IDC_CLEAR, L"BUTTON", L"C&lear log", button);
    add(IDC_PROGRESS, PROGRESS_CLASSW, L"", PBS_SMOOTH);
    add(IDC_STATUS, L"STATIC", L"Ready. CSV records are appended; existing records are kept.", SS_NOPREFIX);
    add(IDC_OUTPUT_LABEL, L"STATIC", L"Result &history:", 0);
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
        dice::Request request;
        request.name = dice::Trim(Text(Control(app, IDC_NAME_EDIT)));
        request.notation = dice::Trim(Text(Control(app, IDC_DICE_EDIT)));
        request.spec = dice::Parse(request.notation);
        request.times = dice::ParseTimes(Text(Control(app, IDC_TIMES_EDIT)));
        request.mode = CurrentMode(app);
        dice::Validate(request);
        settings.name = request.name;
        settings.notation = request.notation;
        settings.times = request.times;
        settings.mode = request.mode;
    } catch (const std::exception&) { /* Keep the last valid roll configuration. */ }
    const auto path = Text(Control(app, IDC_FILE_EDIT));
    try { settings.csvPath = path.empty() ? L"" : dice::ResolvePath(path, app.base).wstring(); }
    catch (const std::exception&) { /* Keep the last valid destination. */ }
    settings.autoSave = Checked(app, IDC_AUTOSAVE);
    settings.showTimestamp = Checked(app, IDC_TIMESTAMP);
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

void RefreshPresets(App& app, const std::wstring& selected) {
    const auto combo = Control(app, IDC_PRESET);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& preset : app.settings.presets)
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(preset.name.c_str()));
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
    SetWindowTextW(Control(app, IDC_DICE_EDIT), preset.notation.c_str());
    SetWindowTextW(Control(app, IDC_TIMES_EDIT), std::to_wstring(preset.times).c_str());
    SetWindowTextW(Control(app, IDC_NAME_EDIT), preset.name.c_str());
    CheckRadioButton(app.window, IDC_RAD_NORMAL, IDC_RAD_DISADV, IDC_RAD_NORMAL + static_cast<int>(preset.mode));
    Status(app, L"Preset applied to the next roll: " + preset.name);
    SetFocus(Control(app, IDC_DICE_EDIT));
    EnsureFocusVisible(app);
}

void SavePreset(App& app) {
    if (Busy(app)) return;
    dice::Preset preset{dice::Trim(Text(Control(app, IDC_PRESET))),
        dice::Trim(Text(Control(app, IDC_DICE_EDIT))), dice::ParseTimes(Text(Control(app, IDC_TIMES_EDIT))), CurrentMode(app)};
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
    SetFocus(Control(app, IDC_DICE_EDIT));
    SendMessageW(Control(app, IDC_DICE_EDIT), EM_SETSEL, 0, -1);
    Status(app, L"Quick die selected. Times and mode are unchanged; select Roll when ready.");
    EnsureFocusVisible(app);
}

void StartRoll(App& app) {
    if (Busy(app) || app.pending) return;
    dice::Request request;
    request.name = dice::Trim(Text(Control(app, IDC_NAME_EDIT)));
    request.notation = dice::Trim(Text(Control(app, IDC_DICE_EDIT)));
    request.spec = dice::Parse(request.notation);
    request.times = dice::ParseTimes(Text(Control(app, IDC_TIMES_EDIT)));
    request.mode = CurrentMode(app);
    request.showTimestamp = Checked(app, IDC_TIMESTAMP);
    dice::Validate(request);
    std::optional<std::filesystem::path> path;
    if (Checked(app, IDC_AUTOSAVE)) path = OutputPath(app);
    app.settings = CaptureSettings(app);
    app.requested = request.times;
    app.saveOnly = false;
    app.job.StartRoll(std::move(request), path);
    SendMessageW(Control(app, IDC_PROGRESS), PBM_SETRANGE32, 0, app.requested);
    SendMessageW(Control(app, IDC_PROGRESS), PBM_SETPOS, 0, 0);
    Status(app, L"Rolling...");
    EnableActions(app);
}

void SavePending(App& app) {
    if (Busy(app) || !app.pending) return;
    const auto path = OutputPath(app);
    app.job.StartSave(app.pending, path);
    app.saveOnly = true;
    Status(app, L"Saving retained results...");
    EnableActions(app);
}

void Poll(App& app) {
    if (!Busy(app)) return;
    const auto phase = app.job.State();
    if (phase != dice::Phase::Finished) {
        if (phase == dice::Phase::Rolling) {
            const auto count = app.job.Completed();
            SendMessageW(Control(app, IDC_PROGRESS), PBM_SETPOS, count, 0);
            Status(app, (app.closing ? L"Cancelling and closing... " : L"Rolling: ") +
                std::to_wstring(count) + L" / " + std::to_wstring(app.requested));
        } else Status(app, app.closing ? L"Waiting for the save to stop safely..." : L"Saving CSV... Results remain in memory until saving succeeds.");
        return;
    }
    auto outcome = app.job.Take();
    const auto count = outcome.batch ? outcome.batch->results.size() : 0;
    // Retain results before display allocation, so display errors cannot lose the batch.
    app.pending = !outcome.saved && count ? outcome.batch : nullptr;
    if (app.closing && FinishClose(app)) return;
    EnableActions(app);
    if (!app.saveOnly && outcome.batch) {
        app.log.Add(L"--- " + outcome.batch->request.name + L": " + std::to_wstring(count) + L" completed roll(s) ---\r\n");
        for (std::size_t i = 0; i < count; ++i)
            app.log.Add(dice::FormatResult(outcome.batch->request, outcome.batch->results[i], i + 1));
        SendMessageW(Control(app, IDC_PROGRESS), PBM_SETPOS, count, 0);
    }
    std::wstring message;
    if (outcome.saved) message = L"Saved " + std::to_wstring(count) + L" roll(s) to: " + outcome.path.wstring();
    else {
        message = outcome.cancelled ? L"Cancelled. " : outcome.error.empty() ? L"Completed. " : L"Operation failed. ";
        message += std::to_wstring(count) + L" roll(s) retained; ";
        message += count ? L"not saved. Choose Save batch or Discard batch." : L"no results to save.";
    }
    app.log.Add(message + L"\r\n");
    RefreshLog(app);
    Status(app, message);
    if (!outcome.error.empty())
        MessageBoxW(app.window, (outcome.error + L"\n\n" + message).c_str(), L"Dice Roller", MB_OK | MB_ICONERROR);
}

struct BrowseStart { std::wstring directory, filename; };
BrowseStart PrepareBrowse(const std::wstring& input, const std::filesystem::path& base) {
    auto candidate = base / L"dice_results_v2.csv";
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
        ? start.filename : L"dice_results_v2.csv";
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
    try { CommitSettings(app, CaptureSettings(app)); }
    catch (const std::exception& error) {
        std::wstring detail = L"The configuration file could not be written.";
        try { detail = dice::Wide(error.what()); } catch (...) {}
        const auto message = L"Settings could not be saved.\n" + detail +
            L"\n\nClose anyway and keep the previously saved settings?";
        if (app.confirm(app.window, message.c_str(), L"Save settings", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
            app.closing = false;
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
    if (Busy(app)) {
        if (app.confirm(app.window, L"Cancel the operation and close? Unsaved results will be discarded.",
            L"Close Dice Roller", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
        // The modal dialog pumps WM_TIMER: Poll may have completed/collected the
        // worker while the question was open. Never reuse the pre-dialog state.
        app.closing = true;
        if (Busy(app)) { app.job.Cancel(); EnableActions(app); }
        else FinishClose(app);
        return;
    }
    if (app.pending && app.confirm(app.window, L"Close and discard the unsaved batch?", L"Close Dice Roller",
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
        switch (msg) {
        case WM_CREATE: Setup(*app); return 0;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED && !app->layingOut) {
                Layout(*app); RememberWindow(*app); EnsureFocusVisible(*app);
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
            case IDC_DISCARD:
                if (!Busy(*app) && app->pending && app->confirm(window, L"Discard this unsaved batch?",
                    L"Discard results", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) == IDYES) {
                    app->pending.reset();
                    Status(*app, L"Unsaved batch discarded. Ready.");
                    EnableActions(*app);
                }
                break;
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
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
        if (!InitCommonControlsEx(&controls)) throw std::runtime_error("Could not initialize Windows controls.");
        App app;
        app.base = dice::DefaultDirectory();
        app.settingsPath = dice::DefaultSettingsPath();
        const auto loaded = dice::LoadSettings(app.settingsPath, dice::DefaultSettings());
        app.settings = loaded.settings;
        app.settingsNeedBackup = !loaded.warning.empty();
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
