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
- Timestamp on new results is initially unchecked and affects newly generated display entries only. CSV always includes the local timestamp.

## Quick dice, presets and remembered settings

- Quick dice buttons offer d4, d6, d8, d10, d12, d20 and d100. They set the expression to one die,
  keep the current Times and Mode, and do not roll until you select **Roll**.
- To create a custom preset, enter its name in the editable **Preset** box, configure Dice/Times/Mode,
  and select **Save preset**. Up to 50 presets are supported, with names up to 64 UTF-16 code units.
- Choose a saved name and select **Use** to restore its expression, times and mode; the roll name becomes
  the preset name. Applying a preset affects the next roll, never a retained unsaved batch.
- Saving an existing name (case-insensitive) asks before replacing it. **Delete preset** also asks for confirmation.
- Preset changes are saved immediately. Normal exit remembers the latest valid roll inputs, CSV path,
  automatic saving, timestamp option, normal window position/size and maximized state.
  An unfinished invalid edit keeps the last valid roll configuration; an empty CSV path can be remembered.
- Preferences and presets are stored in `%LOCALAPPDATA%\DiceRoller\settings.bin`, independently of CSV results.
  The versioned configuration uses UTF-8 text fields with size/boundary checks. Missing settings use defaults;
  unreadable, damaged or unsupported settings use defaults and show a warning in the status area.
- Before replacing a configuration that failed to load, the app preserves its original bytes in a uniquely
  named backup beside `settings.bin`. A failed backup prevents the replacement.
- Configuration writes stage and flush a complete file before replacement. On save failure the old file is preserved;
  preset edits are not committed in memory until persistence succeeds. If settings cannot be saved on exit,
  you may stay in the app or close using the previously saved settings.
- Settings do not contain generated results and do not provide unsaved-batch recovery after application exit.

## Saving and recovery

The default destination is `%LOCALAPPDATA%\DiceRoller\Results\dice_results_v2.csv`.
The full destination is shown in the path field. Relative paths are resolved against that directory, independent of
the process working directory. A missing filename extension gets `.csv`; other extensions are rejected.
Parent directories are created when saving, not when opening a file or validating input.
**Browse** also works when the path is blank or invalid. It starts in an existing directory and only
updates the path after a successful selection; cancelling keeps the original text.

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

- The resizable window scales its layout/font for per-monitor DPI changes. Initial/restored bounds and DPI
  transitions fit the current monitor work area; an unavailable saved monitor falls back to the nearest one.
- Small/high-scale windows scroll the form instead of shrinking text or hiding controls. Action buttons wrap
  to two rows in narrow windows. Scrollbars, mouse wheel and automatic scrolling to keyboard focus keep controls reachable.
- Tab/Shift+Tab navigate controls; arrow keys select roll mode; Enter invokes the current default action.
- Alt+P/B/N/D/T focus the path/browse/name/dice/times controls. Alt+R rolls, Alt+S saves the retained batch,
  Alt+C copies the displayed log, Alt+L clears it. Alt+E focuses Preset, Alt+U applies it, and Alt+H focuses
  Result history. F1 opens About; File > Exit closes the app.
- The display retains up to 500 entries and 120,000 UTF-16 code units. Its footer reports discarded older entries.
  Each displayed group shows at most 30 dice; CSV always retains full details.
- Copy log copies the retained display to the clipboard. Clear log clears only the display, not an unsaved batch or CSV.

## Code structure

- `DiceCore.h/.cpp`: notation parsing, input/workload limits, checked arithmetic, structured results and injectable random source.
- `ResultStore.h/.cpp`: Unicode conversion, formatting, bounded log, stable paths and transactional CSV append.
- `RollJob.h/.cpp`: worker lifetime, progress, cancellation and retention of results after save failure.
- `AppSettings.h/.cpp`: validated presets/preferences, bounded versioned serialization, safe configuration replacement.
- `WindowLayout.h/.cpp`: screen-coordinate window placement and DPI-aware minimum sizes bounded by the monitor work area.
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
Tab/arrow/Enter navigation and saving a retained batch. Additional regressions cover completion while a close confirmation
is active, invalid-path browsing, quick dice/preset lifecycle, settings corruption/save failures and high-scale scrolling.
Actual multi-monitor visual appearance still requires a manual check.
