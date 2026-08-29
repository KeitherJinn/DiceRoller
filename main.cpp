#include "DiceRoller.h"
#include "framework.h"

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <string>
#include <fstream>
#include <random>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")

// Control IDs
#define IDC_FILE_EDIT              100
#define IDC_NAME_EDIT              101
#define IDC_DICE_EDIT              102
#define IDC_TIMES_EDIT             103
#define IDC_RAD_NORMAL             104
#define IDC_RAD_ADV                105
#define IDC_RAD_DISADV             106
#define IDC_SHOW_TIMESTAMP_CHECK   107

#define IDC_ROLL_BUTTON            201
#define IDC_BROWSE_BUTTON          202
#define IDC_OPEN_BUTTON            203

#define IDC_OUTPUT_EDIT            301

// RNG
std::random_device rd;
std::mt19937 gen(rd());

// Roll mode
enum RollMode {
    MODE_NORMAL,
    MODE_ADV,
    MODE_DISADV
};

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ---------- Helpers ----------

// Narrow conversion (simple; fine for ASCIIish text)
std::string Narrow(const std::wstring& ws) {
    return std::string(ws.begin(), ws.end());
}

// Check if file has any content
bool file_has_content(const std::string& filename) {
    std::ifstream in(filename.c_str(), std::ios::in);
    if (!in) return false;
    return in.peek() != std::ifstream::traits_type::eof();
}

// Get text from a wide edit control
std::wstring GetEditText(HWND hEdit) {
    int len = GetWindowTextLengthW(hEdit);
    if (len <= 0) return L"";

    std::wstring text(len, L'\0');
    GetWindowTextW(hEdit, &text[0], len + 1);
    return text;
}

// Convert wstring -> int
bool StringToInt(const std::wstring& s, int& value) {
    try {
        size_t idx = 0;
        int v = std::stoi(s, &idx);

        if (idx != s.size()) return false;

        value = v;
        return true;
    }
    catch (...) {
        return false;
    }
}

// Simple trim
std::wstring Trim(const std::wstring& s) {
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";

    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Parse dice notation like "2d6+3", "d20-1", "3D8", "d12"
bool ParseDiceNotation(
    const std::wstring& notation,
    int& numDice,
    int& sides,
    int& modifier)
{
    std::wstring s = Trim(notation);
    if (s.empty()) return false;

    // Find 'd' or 'D'
    size_t dPos = s.find_first_of(L"dD");
    if (dPos == std::wstring::npos) return false;

    std::wstring left = s.substr(0, dPos);
    std::wstring right = s.substr(dPos + 1);

    // Number of dice
    if (left.empty()) {
        numDice = 1;
    }
    else {
        if (!StringToInt(left, numDice) || numDice <= 0) return false;
    }

    // Look for + or - in the right part
    size_t plusPos = right.find(L'+');
    size_t minusPos = right.find(L'-');

    size_t modPos = std::wstring::npos;
    if (plusPos != std::wstring::npos) {
        modPos = plusPos;
    }
    else if (minusPos != std::wstring::npos) {
        modPos = minusPos;
    }

    std::wstring sidesPart;
    std::wstring modPart;

    if (modPos == std::wstring::npos) {
        sidesPart = right;
        modPart = L"";
    }
    else {
        sidesPart = right.substr(0, modPos);
        modPart = right.substr(modPos); // includes +/-
    }

    sidesPart = Trim(sidesPart);
    modPart = Trim(modPart);

    if (sidesPart.empty()) return false;
    if (!StringToInt(sidesPart, sides) || sides <= 0) return false;

    if (modPart.empty()) {
        modifier = 0;
    }
    else {
        if (!StringToInt(modPart, modifier)) return false;
    }

    return true;
}

// Append text to multi-line output edit
void AppendOutput(HWND hOutput, const std::wstring& text) {
    int len = GetWindowTextLengthW(hOutput);
    SendMessageW(hOutput, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(hOutput, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
}

// Build actual output path: if no folder specified, put under "Results\"
std::wstring BuildOutputPath(const std::wstring& fileNameOnly) {
    // If a path is already present, use as-is
    if (fileNameOnly.find(L'\\') != std::wstring::npos ||
        fileNameOnly.find(L'/') != std::wstring::npos ||
        fileNameOnly.find(L':') != std::wstring::npos)
    {
        return fileNameOnly;
    }

    std::wstring folder = L"Results";
    CreateDirectoryW(folder.c_str(), nullptr); // OK if already exists

    return folder + L"\\" + fileNameOnly;
}

// Roll N dice with given sides, return sum and detail string "(x,y,z)"
int RollNDice(int numDice, int sides, std::wstring& detailOut) {
    std::uniform_int_distribution<int> dist(1, sides);

    int sum = 0;
    std::wostringstream oss;

    oss << L"(";

    for (int i = 0; i < numDice; ++i) {
        int d = dist(gen);
        sum += d;

        oss << d;

        if (i < numDice - 1) {
            oss << L",";
        }
    }

    oss << L")";

    detailOut = oss.str();
    return sum;
}

// ---------- UI setup ----------

void SetupControls(HWND hWnd) {
    // CSV File Name
    CreateWindowW(
        L"STATIC",
        L"CSV File:",
        WS_VISIBLE | WS_CHILD,
        20, 10, 80, 20,
        hWnd,
        nullptr,
        nullptr,
        nullptr
    );

    CreateWindowW(
        L"EDIT",
        L"dice_results.csv",
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
        110, 10, 250, 20,
        hWnd,
        (HMENU)IDC_FILE_EDIT,
        nullptr,
        nullptr
    );

    // Browse button
    CreateWindowW(
        L"BUTTON",
        L"...",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        370, 10, 30, 20,
        hWnd,
        (HMENU)IDC_BROWSE_BUTTON,
        nullptr,
        nullptr
    );

    // Name
    CreateWindowW(
        L"STATIC",
        L"Name:",
        WS_VISIBLE | WS_CHILD,
        20, 40, 80, 20,
        hWnd,
        nullptr,
        nullptr,
        nullptr
    );

    CreateWindowW(
        L"EDIT",
        L"roll",
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
        110, 40, 150, 20,
        hWnd,
        (HMENU)IDC_NAME_EDIT,
        nullptr,
        nullptr
    );

    // Dice notation
    CreateWindowW(
        L"STATIC",
        L"Dice (e.g. 2d6+3):",
        WS_VISIBLE | WS_CHILD,
        20, 70, 130, 20,
        hWnd,
        nullptr,
        nullptr,
        nullptr
    );

    CreateWindowW(
        L"EDIT",
        L"1d100+0",
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
        160, 70, 100, 20,
        hWnd,
        (HMENU)IDC_DICE_EDIT,
        nullptr,
        nullptr
    );

    // Times
    CreateWindowW(
        L"STATIC",
        L"Times:",
        WS_VISIBLE | WS_CHILD,
        20, 100, 80, 20,
        hWnd,
        nullptr,
        nullptr,
        nullptr
    );

    CreateWindowW(
        L"EDIT",
        L"1",
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
        110, 100, 80, 20,
        hWnd,
        (HMENU)IDC_TIMES_EDIT,
        nullptr,
        nullptr
    );

    // Mode group
    HWND hGroup = CreateWindowW(
        L"BUTTON",
        L"Mode",
        WS_VISIBLE | WS_CHILD | BS_GROUPBOX,
        20, 130, 420, 50,
        hWnd,
        nullptr,
        nullptr,
        nullptr
    );

    UNREFERENCED_PARAMETER(hGroup);

    // Normal
    HWND hRadNormal = CreateWindowW(
        L"BUTTON",
        L"Normal",
        WS_VISIBLE | WS_CHILD | BS_RADIOBUTTON | WS_GROUP | WS_TABSTOP,
        30, 150, 100, 20,
        hWnd,
        (HMENU)IDC_RAD_NORMAL,
        nullptr,
        nullptr
    );

    // Advantage
    CreateWindowW(
        L"BUTTON",
        L"Advantage",
        WS_VISIBLE | WS_CHILD | BS_RADIOBUTTON,
        140, 150, 100, 20,
        hWnd,
        (HMENU)IDC_RAD_ADV,
        nullptr,
        nullptr
    );

    // Disadvantage
    CreateWindowW(
        L"BUTTON",
        L"Disadvantage",
        WS_VISIBLE | WS_CHILD | BS_RADIOBUTTON,
        260, 150, 120, 20,
        hWnd,
        (HMENU)IDC_RAD_DISADV,
        nullptr,
        nullptr
    );

    // Default mode: Normal
    SendMessageW(hRadNormal, BM_SETCHECK, BST_CHECKED, 0);

    // Show timestamp checkbox
    HWND hShowTimestamp = CreateWindowW(
        L"BUTTON",
        L"Show timestamp",
        WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
        300, 105, 130, 20,
        hWnd,
        (HMENU)IDC_SHOW_TIMESTAMP_CHECK,
        nullptr,
        nullptr
    );

    // Default: checked
    SendMessageW(hShowTimestamp, BM_SETCHECK, BST_UNCHECKED, 0);

    // Roll button
    CreateWindowW(
        L"BUTTON",
        L"Roll",
        WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        450, 145, 80, 24,
        hWnd,
        (HMENU)IDC_ROLL_BUTTON,
        nullptr,
        nullptr
    );

    // Open CSV button
    CreateWindowW(
        L"BUTTON",
        L"Open CSV",
        WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        540, 145, 80, 24,
        hWnd,
        (HMENU)IDC_OPEN_BUTTON,
        nullptr,
        nullptr
    );

    // Output multi-line edit
    CreateWindowW(
        L"EDIT",
        L"",
        WS_VISIBLE | WS_CHILD | WS_BORDER | ES_MULTILINE |
        ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        20, 200, 600, 240,
        hWnd,
        (HMENU)IDC_OUTPUT_EDIT,
        nullptr,
        nullptr
    );
}

// ---------- Logic ----------

void HandleRoll(HWND hWnd) {
    HWND hFile = GetDlgItem(hWnd, IDC_FILE_EDIT);
    HWND hName = GetDlgItem(hWnd, IDC_NAME_EDIT);
    HWND hDice = GetDlgItem(hWnd, IDC_DICE_EDIT);
    HWND hTimes = GetDlgItem(hWnd, IDC_TIMES_EDIT);
    HWND hOutput = GetDlgItem(hWnd, IDC_OUTPUT_EDIT);
    HWND hShowTimestamp = GetDlgItem(hWnd, IDC_SHOW_TIMESTAMP_CHECK);

    bool showTimestamp =
        SendMessageW(hShowTimestamp, BM_GETCHECK, 0, 0) == BST_CHECKED;

    std::wstring filenameW = GetEditText(hFile);
    if (filenameW.empty()) {
        MessageBoxW(hWnd, L"Please enter a CSV filename.", L"Error", MB_ICONERROR);
        return;
    }

    // Ensure .csv extension if missing any dot
    if (filenameW.find(L'.') == std::wstring::npos) {
        filenameW += L".csv";
        SetWindowTextW(hFile, filenameW.c_str());
    }

    // Put file under "Results\" if no explicit path
    std::wstring fullPathW = BuildOutputPath(filenameW);

    std::wstring nameW = GetEditText(hName);
    if (nameW.empty()) {
        MessageBoxW(hWnd, L"Please enter a name.", L"Input Error", MB_ICONWARNING);
        return;
    }

    std::wstring diceStrW = Trim(GetEditText(hDice));
    if (diceStrW.empty()) {
        MessageBoxW(
            hWnd,
            L"Please enter dice notation like 2d6+3 or d20-1.",
            L"Input Error",
            MB_ICONWARNING
        );
        return;
    }

    int times = 0;
    if (!StringToInt(GetEditText(hTimes), times) || times <= 0) {
        MessageBoxW(
            hWnd,
            L"Times must be a positive integer.",
            L"Input Error",
            MB_ICONWARNING
        );
        return;
    }

    int numDice = 0;
    int sides = 0;
    int modifier = 0;

    if (!ParseDiceNotation(diceStrW, numDice, sides, modifier)) {
        MessageBoxW(
            hWnd,
            L"Invalid dice notation. Use forms like 2d6+3, d20-1, 3d8.",
            L"Dice Error",
            MB_ICONWARNING
        );
        return;
    }

    // Determine mode from radio buttons
    RollMode mode = MODE_NORMAL;

    if (SendMessageW(GetDlgItem(hWnd, IDC_RAD_ADV), BM_GETCHECK, 0, 0) == BST_CHECKED) {
        mode = MODE_ADV;
    }
    else if (SendMessageW(GetDlgItem(hWnd, IDC_RAD_DISADV), BM_GETCHECK, 0, 0) == BST_CHECKED) {
        mode = MODE_DISADV;
    }

    std::string filename = Narrow(fullPathW);
    std::string name = Narrow(nameW);
    std::string diceStr = Narrow(diceStrW);

    bool hasContent = file_has_content(filename);

    std::ofstream out(filename, std::ios::app);
    if (!out) {
        MessageBoxW(
            hWnd,
            L"Failed to open CSV file for writing.",
            L"File Error",
            MB_ICONERROR
        );
        return;
    }

    if (!hasContent) {
        out << "Name,DiceNotation,Mode,NumDice,Sides,BaseRoll,AltRoll,Modifier,FinalResult,Timestamp\n";
    }

    std::string modeStr;

    switch (mode) {
    case MODE_NORMAL:
        modeStr = "Normal";
        break;

    case MODE_ADV:
        modeStr = "Advantage";
        break;

    case MODE_DISADV:
        modeStr = "Disadvantage";
        break;
    }

    for (int i = 0; i < times; ++i) {
        // First roll
        std::wstring detail1;
        std::wstring detail2;

        int roll1 = RollNDice(numDice, sides, detail1);
        int roll2 = 0;
        int chosen = roll1;

        if (mode == MODE_ADV || mode == MODE_DISADV) {
            roll2 = RollNDice(numDice, sides, detail2);

            if (mode == MODE_ADV) {
                chosen = (roll1 >= roll2) ? roll1 : roll2;
            }
            else {
                chosen = (roll1 <= roll2) ? roll1 : roll2;
            }
        }

        // Timestamp
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);

        std::tm tm_buf;
        localtime_s(&tm_buf, &t);

        wchar_t tsW[64];
        wcsftime(
            tsW,
            sizeof(tsW) / sizeof(wchar_t),
            L"%Y-%m-%d %H:%M:%S",
            &tm_buf
        );

        char tsA[64];
        std::strftime(
            tsA,
            sizeof(tsA),
            "%Y-%m-%d %H:%M:%S",
            &tm_buf
        );

        int finalResult = chosen + modifier;

        // CSV
        out << "\"" << name << "\","
            << "\"" << diceStr << "\","
            << "\"" << modeStr << "\","
            << numDice << ","
            << sides << ","
            << chosen << ",";

        if (mode == MODE_NORMAL) {
            out << ",";
        }
        else {
            int alt = (chosen == roll1 ? roll2 : roll1);
            out << alt << ",";
        }

        out << modifier << ","
            << finalResult << ",\""
            << tsA << "\"\n";

        // GUI log
        std::wostringstream line;

        line << L"Roll " << (i + 1) << L" for " << nameW
            << L" [" << diceStrW << L"] ";

        if (mode == MODE_NORMAL) {
            line << detail1 << L" = " << roll1;
        }
        else {
            line << L"rolls " << detail1 << L" = " << roll1
                << L", " << detail2 << L" = " << roll2
                << L" -> chosen " << chosen
                << L" (" << (mode == MODE_ADV ? L"advantage" : L"disadvantage") << L")";
        }

		if (modifier != 0) {
            line << L" (modifier " << (modifier >= 0 ? L"+" : L"") << modifier;
		}

        line << L") -> Final: " << finalResult;

        if (showTimestamp) {
            line << L"  @ " << tsW;
        }

        line << L"\r\n";

        AppendOutput(hOutput, line.str());
    }

    out.flush();

    std::wstring msg = L"Roll(s) saved to:\n" + fullPathW;
}

void HandleBrowse(HWND hWnd) {
    wchar_t fileBuffer[MAX_PATH] = L"";

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = L"csv";

    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(GetDlgItem(hWnd, IDC_FILE_EDIT), fileBuffer);
    }
}

void HandleOpenCsv(HWND hWnd) {
    HWND hFile = GetDlgItem(hWnd, IDC_FILE_EDIT);

    std::wstring filenameW = GetEditText(hFile);
    if (filenameW.empty()) {
        MessageBoxW(
            hWnd,
            L"Please enter or choose a CSV file first.",
            L"Error",
            MB_ICONERROR
        );
        return;
    }

    if (filenameW.find(L'.') == std::wstring::npos) {
        filenameW += L".csv";
        SetWindowTextW(hFile, filenameW.c_str());
    }

    std::wstring fullPathW = BuildOutputPath(filenameW);

    HINSTANCE res = ShellExecuteW(
        hWnd,
        L"open",
        fullPathW.c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL
    );

    if ((INT_PTR)res <= 32) {
        MessageBoxW(
            hWnd,
            L"Failed to open CSV file. Check that it exists and has an associated app.",
            L"Open Error",
            MB_ICONERROR
        );
    }
}

// ---------- Window procedure & entry ----------

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        SetupControls(hWnd);
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_ROLL_BUTTON:
            HandleRoll(hWnd);
            break;

        case IDC_BROWSE_BUTTON:
            HandleBrowse(hWnd);
            break;

        case IDC_OPEN_BUTTON:
            HandleOpenCsv(hWnd);
            break;

        case IDC_RAD_NORMAL:
        case IDC_RAD_ADV:
        case IDC_RAD_DISADV:
            // Make radios mutually exclusive
            CheckRadioButton(hWnd, IDC_RAD_NORMAL, IDC_RAD_DISADV, LOWORD(wParam));
            break;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    return 0;
}

// Unicode entry point
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    WNDCLASSW wc = {};

    wc.hInstance = hInst;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"DiceGUIClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc)) {
        MessageBoxW(
            nullptr,
            L"Failed to register window class.",
            L"Error",
            MB_ICONERROR
        );
        return 1;
    }

    HWND hWnd = CreateWindowW(
        L"DiceGUIClass",
        L"Dice Roller GUI (Dice Notation + Advantage/Disadvantage)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        680,
        520,
        nullptr,
        nullptr,
        hInst,
        nullptr
    );

    if (!hWnd) {
        MessageBoxW(
            nullptr,
            L"Failed to create window.",
            L"Error",
            MB_ICONERROR
        );
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;

    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}