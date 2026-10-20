# Dice Roller

A C++17 / Win32 desktop dice roller for Windows 10 version 1703 or later and Windows 11.
It uses the Windows SDK and the MSVC v143 toolset; no third-party libraries or package downloads are required.

## Rolling dice

- Enter a name, notation such as `2d6+3`, `d20-1`, `3D8` or `d12`, and the number of rolls.
- Normal generates one complete dice group. Advantage generates two complete groups and keeps the higher total;
  Disadvantage keeps the lower total. This is **whole-group comparison**, not a separate comparison for each die.
- The modifier is applied once to the selected group total. Both groups and their original order are retained.
- Limits: 1–1,000 dice per group; 1–2,147,483,647 sides; 1–10,000 rolls;
  at most 1,000,000 generated dice per batch (counting both groups for Advantage/Disadvantage).
- Results and modifiers use signed 64-bit integers. Inputs whose possible result exceeds that range are rejected.
- Computation and saving run on a worker thread. The progress bar shows completed rolls; Cancel/Escape stops the operation.
  Only fully completed rolls are retained when calculation is cancelled. A partially generated group is discarded.
- Show timestamp is initially unchecked and changes the display only. CSV always includes the local timestamp.

## Saving and recovery

The default destination is `%LOCALAPPDATA%\DiceRoller\Results\dice_results_v2.csv`.
The full destination is shown in the path field. Relative paths are resolved against that directory, independent of
the process working directory. A missing filename extension gets `.csv`; other extensions are rejected.
Parent directories are created when saving, not when opening a file or validating input.

Automatic append is on by default. Uncheck it to roll without writing a file, then use **Save batch** when ready.
Existing compatible CSV records are preserved and new records are appended; the file picker does not promise to overwrite them.

Saving stages the complete updated file beside the destination, checks every write and flushes it to disk before replacing
the destination. A write/flush/commit failure or cancellation before commit leaves the original CSV unchanged.
Temporary files and the operation's lock are removed on normal failure/cancellation. A pre-existing `.lock` companion
is never overwritten or deleted. A force-killed process can leave an unused `.tmp` staging file.
The replacement requires temporary free disk space roughly equal to the existing CSV plus the new records.
Do not edit the destination externally while a save is in progress.

On failure, completed results remain in memory. Change the destination if needed and select **Save batch** to retry
the same results without rolling again. New rolls are disabled while an unsaved batch exists; save or explicitly discard
it first. Cancellation during saving retains the full batch for retry. Once the final replacement completes, the save
is successful even if Cancel was pressed too late to stop it.
Closing with an active operation or unsaved batch requires confirmation. Unsaved results do not survive application exit.

CSV files use UTF-8 (new files include a BOM for spreadsheet compatibility), CRLF record separators and proper double-quote escaping.
Format v2 columns are:

`Name,DiceNotation,Mode,NumDice,Sides,Roll1,Roll2,ChosenRoll,Modifier,FinalResult,Timestamp,Roll1Dice,Roll2Dice`

`Roll1` and `Roll2` preserve generation order; `ChosenRoll` is the selected total before the modifier.
Normal mode leaves `Roll2` and `Roll2Dice` empty. The dice-list fields retain every die, even when the on-screen detail is shortened.
Appending validates the header (with or without a UTF-8 BOM) and repairs a missing final record newline.
Legacy `BaseRoll,AltRoll` files and unrelated files are rejected without modification. Choose a new v2 file;
the old records cannot reliably recover the original order and are not silently converted.

## Window and keyboard

- The resizable window uses a minimum client size and scales its layout/font for per-monitor DPI changes.
- Tab/Shift+Tab navigate controls; arrow keys select roll mode; Enter invokes the current default action.
- Alt+P/B/N/D/T focus the path/browse/name/dice/times controls. Alt+R rolls, Alt+S saves the retained batch,
  Alt+C copies the displayed log, Alt+L clears it. F1 opens About; File > Exit closes the app.
- The display retains up to 500 entries and 120,000 UTF-16 code units. Its footer reports discarded older entries.
  Each displayed group shows at most 30 dice; CSV always retains full details.
- Copy log copies the retained display to the clipboard. Clear log clears only the display, not an unsaved batch or CSV.

## Code structure

- `DiceCore.h/.cpp`: notation parsing, input/workload limits, checked arithmetic, structured results and injectable random source.
- `ResultStore.h/.cpp`: Unicode conversion, formatting, bounded log, stable paths and transactional CSV append.
- `RollJob.h/.cpp`: worker lifetime, progress, cancellation and retention of results after save failure.
- `main.cpp`: Win32 controls, layout, input routing and UI state. It polls the worker; worker code never accesses an HWND.
- `DiceRoller.rc`, `Resource.h`, `DiceRoller.manifest`: menu, About dialog, icons, IDs, visual styles and DPI declaration.
- `tests/`: dependency-free regression executable, including tests of the application's own hidden Win32 windows.

## Build and test

Open `DiceRoller.sln` in Visual Studio with **Desktop development with C++**, the v143 toolset and a Windows 10/11 SDK installed.
The solution contains the application and `DiceRoller.Tests` and supports Debug/Release for x64/x86.
From a Visual Studio developer command prompt, for example:

```bat
msbuild DiceRoller.sln /p:Configuration=Debug /p:Platform=x64 /m
x64\Debug\DiceRoller.Tests.exe
msbuild DiceRoller.sln /p:Configuration=Release /p:Platform=x64 /m
x64\Release\DiceRoller.Tests.exe
```

For 32-bit solution builds use `/p:Platform=x86`. The application is written to `Debug\` or `Release\`;
the 32-bit tests are written to `Win32\Debug\` or `Win32\Release\`.
Tests report failures with a nonzero exit code and clean up their own uniquely named temporary directories.
They cover deterministic rules, invalid input, numerical boundaries, Unicode, CSV compatibility/append/failure recovery,
cancellation, worker state, log retention, hidden-window resizing, simulated 96/144/192 DPI layout,
Tab/arrow/Enter navigation and saving a retained batch. Actual multi-monitor visual appearance still requires a manual check.
