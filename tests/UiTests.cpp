// Exercise our own hidden Win32 windows in-process. No input is injected into the desktop.
#include "../main.cpp"
#include <chrono>

namespace {
void CheckUi(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct TestWindow {
    App app;
    ATOM type = 0;
    explicit TestWindow(const std::filesystem::path& base) {
        app.base = base;
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
        CheckUi(CreateWindowExW(0, wc.lpszClassName, L"Dice Roller integration test", WS_OVERLAPPEDWINDOW,
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
    AdjustWindowRectExForDpi(&window, WS_OVERLAPPEDWINDOW, TRUE, 0, GetDpiForWindow(app.window));
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
        ResizeClient(app, 780, 550, dpi);
        RECT client{};
        GetClientRect(app.window, &client);
        for (HWND child = GetWindow(app.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            const auto bounds = Bounds(child, app.window);
            CheckUi(bounds.left >= 0 && bounds.top >= 0 && bounds.right <= client.right && bounds.bottom <= client.bottom,
                "A control falls outside the minimum client area.");
        }
        const auto oldLog = Bounds(Control(app, IDC_OUTPUT), app.window);
        ResizeClient(app, 1000, 800, dpi);
        const auto newLog = Bounds(Control(app, IDC_OUTPUT), app.window);
        CheckUi(newLog.right - newLog.left > oldLog.right - oldLog.left &&
            newLog.bottom - newLog.top > oldLog.bottom - oldLog.top, "Log does not grow with the window.");
        LOGFONTW font{};
        GetObjectW(app.font, sizeof(font), &font);
        CheckUi(font.lfHeight == -MulDiv(10, static_cast<int>(dpi), 72), "Font was not rescaled.");
        auto previous = Bounds(Control(app, IDC_ROLL), app.window);
        for (int id : {IDC_CANCEL, IDC_SAVE, IDC_DISCARD, IDC_OPEN, IDC_COPY, IDC_CLEAR}) {
            const auto next = Bounds(Control(app, id), app.window);
            CheckUi(previous.right <= next.left, "Action buttons overlap.");
            previous = next;
        }
    }
    ResizeClient(app, 840, 680, 96);
    const int tabOrder[] = {IDC_FILE_EDIT, IDC_BROWSE, IDC_NAME_EDIT, IDC_DICE_EDIT, IDC_TIMES_EDIT, IDC_RAD_NORMAL};
    for (int id : tabOrder) CheckUi(GetWindowLongPtrW(Control(app, id), GWL_STYLE) & WS_TABSTOP, "Missing tab stop.");
    SetFocus(Control(app, IDC_FILE_EDIT));
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

    CheckRadioButton(app.window, IDC_RAD_NORMAL, IDC_RAD_DISADV, IDC_RAD_NORMAL);
    SetWindowTextW(Control(app, IDC_DICE_EDIT), L"1d1+0");
    SetWindowTextW(Control(app, IDC_TIMES_EDIT), L"2");
    SendMessageW(Control(app, IDC_AUTOSAVE), BM_SETCHECK, BST_UNCHECKED, 0);
    SetFocus(Control(app, IDC_DICE_EDIT));
    MSG enter{};
    enter.hwnd = GetFocus(); enter.message = WM_KEYDOWN; enter.wParam = VK_RETURN;
    IsDialogMessageW(app.window, &enter);
    CheckUi(Busy(app) && !IsWindowEnabled(Control(app, IDC_ROLL)), "Enter did not start a background operation.");
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
