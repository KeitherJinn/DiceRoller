#include "framework.h"
#include "Resource.h"
#include "RollJob.h"
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
struct App {
    HWND window = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
    int defaultButton = IDC_ROLL;
    int requested = 0;
    bool saveOnly = false;
    bool closing = false;
    bool logFailed = false;
    std::filesystem::path base;
    std::shared_ptr<const dice::Batch> pending;
    dice::LogBuffer log;
    dice::RollJob job;
    ~App() { if (font) DeleteObject(font); }
};
HWND Control(const App& app, int id) { return GetDlgItem(app.window, id); }
bool Checked(const App& app, int id) { return SendMessageW(Control(app, id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
bool Busy(const App& app) { return app.job.State() != dice::Phase::Idle; }
int Px(const App& app, int value) { return MulDiv(value, static_cast<int>(app.dpi), 96); }

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
                   IDC_AUTOSAVE, IDC_BROWSE, IDC_OPEN})
        EnableWindow(Control(app, id), !busy);
    EnableWindow(Control(app, IDC_ROLL), !busy && !app.pending);
    EnableWindow(Control(app, IDC_CANCEL), busy && !app.closing);
    EnableWindow(Control(app, IDC_SAVE), !busy && static_cast<bool>(app.pending));
    EnableWindow(Control(app, IDC_DISCARD), !busy && static_cast<bool>(app.pending));
    const int focus = busy ? IDC_CANCEL : app.pending ? IDC_SAVE : IDC_ROLL;
    SetFocus(Control(app, focus));
    SendMessageW(app.window, DM_SETDEFID, focus, 0);
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
    RECT area{};
    GetClientRect(app.window, &area);
    const int width = MulDiv(area.right, 96, static_cast<int>(app.dpi));
    const int height = MulDiv(area.bottom, 96, static_cast<int>(app.dpi));
    auto move = [&](int id, int x, int y, int w, int h) {
        MoveWindow(Control(app, id), Px(app, x), Px(app, y), Px(app, (std::max)(1, w)), Px(app, (std::max)(1, h)), TRUE);
    };
    move(IDC_FILE_LABEL, 16, 20, 80, 24);
    move(IDC_FILE_EDIT, 100, 16, width - 224, 26);
    move(IDC_BROWSE, width - 108, 16, 92, 26);
    move(IDC_NAME_LABEL, 16, 56, 80, 24);
    move(IDC_NAME_EDIT, 100, 52, width - 116, 26);
    move(IDC_DICE_LABEL, 16, 92, 80, 24);
    move(IDC_DICE_EDIT, 100, 88, width - 330, 26);
    move(IDC_TIMES_LABEL, width - 208, 92, 70, 24);
    move(IDC_TIMES_EDIT, width - 128, 88, 112, 26);
    move(IDC_MODE_GROUP, 16, 124, width - 32, 62);
    move(IDC_RAD_NORMAL, 32, 148, 130, 24);
    move(IDC_RAD_ADV, 180, 148, 160, 24);
    move(IDC_RAD_DISADV, 360, 148, 180, 24);
    move(IDC_RULES, 16, 196, width - 32, 40);
    move(IDC_TIMESTAMP, 16, 242, 170, 24);
    move(IDC_AUTOSAVE, 206, 242, 360, 24);
    move(IDC_ROLL, 16, 278, 88, 28);
    move(IDC_CANCEL, 112, 278, 88, 28);
    move(IDC_SAVE, 208, 278, 104, 28);
    move(IDC_DISCARD, 320, 278, 104, 28);
    move(IDC_OPEN, width - 308, 278, 92, 28);
    move(IDC_COPY, width - 208, 278, 92, 28);
    move(IDC_CLEAR, width - 108, 278, 92, 28);
    move(IDC_PROGRESS, 16, 318, width - 32, 18);
    move(IDC_STATUS, 16, 346, width - 32, 40);
    move(IDC_OUTPUT, 16, 394, width - 32, height - 432);
    move(IDC_RETENTION, 16, height - 30, width - 32, 24);
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
        return control;
    };
    const DWORD edit = WS_TABSTOP | ES_AUTOHSCROLL;
    const DWORD button = WS_TABSTOP | BS_PUSHBUTTON;
    add(IDC_FILE_LABEL, L"STATIC", L"CSV &path:", 0);
    auto file = add(IDC_FILE_EDIT, L"EDIT", (app.base / L"dice_results_v2.csv").c_str(), edit, WS_EX_CLIENTEDGE);
    SendMessageW(file, EM_LIMITTEXT, 32760, 0);
    add(IDC_BROWSE, L"BUTTON", L"&Browse...", button);
    add(IDC_NAME_LABEL, L"STATIC", L"&Name:", 0);
    SendMessageW(add(IDC_NAME_EDIT, L"EDIT", L"roll", edit, WS_EX_CLIENTEDGE), EM_LIMITTEXT, 128, 0);
    add(IDC_DICE_LABEL, L"STATIC", L"&Dice:", 0);
    SendMessageW(add(IDC_DICE_EDIT, L"EDIT", L"1d100+0", edit, WS_EX_CLIENTEDGE), EM_LIMITTEXT, 128, 0);
    add(IDC_TIMES_LABEL, L"STATIC", L"&Times:", 0);
    SendMessageW(add(IDC_TIMES_EDIT, L"EDIT", L"1", edit | ES_NUMBER, WS_EX_CLIENTEDGE), EM_LIMITTEXT, 5, 0);
    add(IDC_MODE_GROUP, L"BUTTON", L"Mode", BS_GROUPBOX);
    add(IDC_RAD_NORMAL, L"BUTTON", L"N&ormal", BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP);
    add(IDC_RAD_ADV, L"BUTTON", L"&Advantage", BS_AUTORADIOBUTTON);
    add(IDC_RAD_DISADV, L"BUTTON", L"Disad&vantage", BS_AUTORADIOBUTTON);
    CheckRadioButton(app.window, IDC_RAD_NORMAL, IDC_RAD_DISADV, IDC_RAD_NORMAL);
    add(IDC_RULES, L"STATIC", L"Advantage / Disadvantage: roll the entire dice group twice; keep the higher / lower total.\r\n"
        L"Limits: 1,000 dice per group; 10,000 rolls; 1,000,000 generated dice per batch.", SS_NOPREFIX | WS_GROUP);
    add(IDC_TIMESTAMP, L"BUTTON", L"Show timestamp", BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP);
    // Timestamp display defaults to off; timestamps are always recorded in CSV.
    SendMessageW(Control(app, IDC_TIMESTAMP), BM_SETCHECK, BST_UNCHECKED, 0);
    add(IDC_AUTOSAVE, L"BUTTON", L"Automatically append completed batch to CSV", BS_AUTOCHECKBOX | WS_TABSTOP);
    SendMessageW(Control(app, IDC_AUTOSAVE), BM_SETCHECK, BST_CHECKED, 0);
    add(IDC_ROLL, L"BUTTON", L"&Roll", WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON);
    add(IDC_CANCEL, L"BUTTON", L"Cancel", button);
    add(IDC_SAVE, L"BUTTON", L"&Save batch", button);
    add(IDC_DISCARD, L"BUTTON", L"Discard batch", button);
    add(IDC_OPEN, L"BUTTON", L"Open CSV", button);
    add(IDC_COPY, L"BUTTON", L"&Copy log", button);
    add(IDC_CLEAR, L"BUTTON", L"C&lear log", button);
    add(IDC_PROGRESS, PROGRESS_CLASSW, L"", PBS_SMOOTH);
    add(IDC_STATUS, L"STATIC", L"Ready. CSV records are appended; existing records are kept.", SS_NOPREFIX);
    add(IDC_OUTPUT, L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, WS_EX_CLIENTEDGE);
    SendMessageW(Control(app, IDC_OUTPUT), EM_LIMITTEXT, dice::LogBuffer::MaxCharacters, 0);
    add(IDC_RETENTION, L"STATIC", L"", SS_NOPREFIX);
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

void StartRoll(App& app) {
    if (Busy(app) || app.pending) return;
    dice::Request request;
    request.name = dice::Trim(Text(Control(app, IDC_NAME_EDIT)));
    request.notation = dice::Trim(Text(Control(app, IDC_DICE_EDIT)));
    request.spec = dice::Parse(request.notation);
    request.times = dice::ParseTimes(Text(Control(app, IDC_TIMES_EDIT)));
    request.mode = Checked(app, IDC_RAD_ADV) ? dice::Mode::Advantage
        : Checked(app, IDC_RAD_DISADV) ? dice::Mode::Disadvantage : dice::Mode::Normal;
    request.showTimestamp = Checked(app, IDC_TIMESTAMP);
    dice::Validate(request);
    std::optional<std::filesystem::path> path;
    if (Checked(app, IDC_AUTOSAVE)) path = OutputPath(app);
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
    if (app.closing) { DestroyWindow(app.window); return; }
    const auto count = outcome.batch ? outcome.batch->results.size() : 0;
    // Retain results before display allocation, so display errors cannot lose the batch.
    app.pending = !outcome.saved && count ? outcome.batch : nullptr;
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

void Browse(App& app) {
    if (Busy(app)) return;
    std::vector<wchar_t> buffer(32768, L'\0');
    const auto current = OutputPath(app).wstring();
    if (current.size() >= buffer.size()) throw std::invalid_argument("The path is too long for the file picker.");
    std::copy(current.begin(), current.end(), buffer.begin());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app.window;
    ofn.lpstrFilter = L"CSV files (*.csv)\0*.csv\0";
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrDefExt = L"csv";
    ofn.lpstrTitle = L"Choose CSV to append to (existing records are kept)";
    ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetSaveFileNameW(&ofn)) SetWindowTextW(Control(app, IDC_FILE_EDIT), buffer.data());
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
void Close(App& app) {
    if (app.closing) return;
    if (Busy(app)) {
        if (MessageBoxW(app.window, L"Cancel the operation and close? Unsaved results will be discarded.",
            L"Close Dice Roller", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
        app.closing = true;
        app.job.Cancel();
        EnableActions(app);
        return;
    }
    if (app.pending && MessageBoxW(app.window, L"Close and discard the unsaved batch?", L"Close Dice Roller",
        MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION) != IDYES) return;
    DestroyWindow(app.window);
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
        case WM_SIZE: if (wParam != SIZE_MINIMIZED) Layout(*app); return 0;
        case WM_DPICHANGED: {
            app->dpi = HIWORD(wParam);
            SetFont(*app);
            const auto rect = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(*app);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            RECT rect{0, 0, Px(*app, 780), Px(*app, 550)};
            AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, TRUE, 0, app->dpi);
            auto limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = {rect.right - rect.left, rect.bottom - rect.top};
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
            switch (id) {
            case IDOK: case IDC_ROLL: StartRoll(*app); break;
            case IDCANCEL: case IDC_CANCEL: if (Busy(*app)) app->job.Cancel(); break;
            case IDC_SAVE: SavePending(*app); break;
            case IDC_DISCARD:
                if (!Busy(*app) && app->pending && MessageBoxW(window, L"Discard this unsaved batch?",
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
        RECT rect{0, 0, MulDiv(840, static_cast<int>(dpi), 96), MulDiv(680, static_cast<int>(dpi), 96)};
        AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, TRUE, 0, dpi);
        const auto window = CreateWindowExW(0, wc.lpszClassName, L"Dice Roller", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, instance, &app);
        if (!window) return 1;
        const auto accelerators = LoadAcceleratorsW(instance, MAKEINTRESOURCEW(IDC_DICEROLLER));
        ShowWindow(window, show);
        UpdateWindow(window);
        MSG msg{};
        int status = 0;
        while ((status = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            if (!TranslateAcceleratorW(window, accelerators, &msg) && !IsDialogMessageW(window, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (status == -1) throw std::runtime_error("The Windows message loop failed.");
        return static_cast<int>(msg.wParam);
    } catch (const std::exception& error) { Error(nullptr, error); return 1; }
}
