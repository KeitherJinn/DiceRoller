# Dice Roller

A C++17 / Win32 desktop dice roller for Windows 10 version 1703 or later and Windows 11.
It uses the Windows SDK and MSVC v143; no third-party libraries or package downloads are required.

## Features

Ten feature groups extend quick dice, custom presets, remembered settings and DPI-aware windows:

| Feature | Behavior |
| --- | --- |
| Complete history | Structured batches, name/formula/UTC date/export-status filters, and complete per-roll details. |
| Target checks | Final-value comparisons using `>=`, `<=`, `=`, `>` or `<`, with success margins. |
| Batch statistics | Count, minimum, maximum, mean, median, population standard deviation, target matches and frequency charts. |
| Mixed expressions | Different dice terms in one expression, such as `2d6+1d4+3`. |
| Keep rules | `kh`/`kl` retain the highest/lowest dice while preserving dropped dice in the record. |
| Success-count pools | Count retained dice meeting a threshold instead of summing their points. |
| Recovery checkpoints | Restore completed rolls from the last committed local checkpoint after restart. |
| Preset groups and exchange | Group filtering and `.drpresets` import/export with duplicate previews and merge choices. |
| Probability comparison | Compare expressions using bounded exact calculation or clearly labelled simulation. |
| Continuous batches and combined export | Keep rolling while earlier batches await export; export selected batches together without duplicating identical batch IDs. |

## Rolling dice and rule options

Enter a name, expression and **Times**, choose a mode, then select **Roll**.

| Example | Meaning |
| --- | --- |
| `2d6+3`, `d20-1`, `3D8`, `d12` | Ordinary dice and optional integer modifiers. `D` is case-insensitive; omitted count means one die. |
| `2d6+1d4+3` | Two d6 plus one d4, then add three. |
| `4d6kh3` / `4d6kl2` | Roll four d6 and retain the highest three / lowest two. |
| `4d6kh3+1d4+2` | Apply the keep rule to its own term, add the other term, then apply the modifier. |
| `8d10`, **Pool: each die >= 8** | Count the retained dice showing eight or more. |
| `d20+5`, **Target >= 15** | Judge the final modified total against fifteen. |

- Normal generates one complete group. Advantage generates two complete groups and keeps the higher group result;
  Disadvantage keeps the lower. Each group contains every expression term and its keep rules. This is whole-group
  comparison, not separate advantage for each die. Both groups retain their original generation order.
- `khN`/`klN` retain one to the term's generated number of dice. Ties retain generation order. Dropped dice remain
  visible in History and export, but do not contribute to the result.
- Integer modifiers apply once after group selection. Dice terms can be added; subtraction applies to integer
  modifiers, not negative dice terms. Parentheses, multiplication, reroll and exploding rules are not supported.
- Pool mode counts only retained dice meeting its threshold; advantage/disadvantage compares success counts.
  A threshold above a die's sides produces no successes for that die. **Pool mode rejects nonzero modifiers**:
  use `8d10`, not `8d10+2`, while Pool is enabled.
- Target checks use the final total, or final success count in Pool mode. No game-specific natural-20 rule is assumed.
  A nonnegative margin meets the comparison; strict `>`/`<` include the one-integer step. Equality reports zero for
  a match and a negative distance otherwise.
- Limits: 1–1,000 generated dice across all terms per group; 1–2,147,483,647 sides; 1–10,000 rolls; at most 1,000,000
  generated dice per batch, including both groups in Advantage/Disadvantage. The UI accepts expressions up to 1,024
  UTF-16 code units. Results/modifiers are signed 64-bit integers; inputs with unsafe possible results are rejected.
- Rolling and export run on a worker thread. Progress counts completed rolls. Cancel/Escape retains completed rolls;
  a partially generated group is discarded. Previous results are never rerolled by cancellation or retry.
- **Timestamp on new results** initially starts unchecked and affects new recent-log entries only. Full history and
  CSV retain each roll's local timestamp; batch creation times are separately recorded in UTC.

## History, statistics and probability

Select **History...** to browse locally recorded batches. Name and formula filters match substrings without regard
to case. Inclusive UTC date filters use `YYYY-MM-DD`; blank dates are unbounded. **Pending export only** filters by
CSV export status. Completed/requested counts also identify partially completed batches.

Select a batch and use **Previous**, **Next**, or **Roll #** to inspect any completed roll. Details retain every die,
its term/sides and retained/dropped state, both groups, target outcome and totals. Square brackets identify dropped
dice. This view is complete even when the recent log is shortened. Read-only text fields support selection/copying.

Ctrl/Shift selects multiple batches. Statistics and normalized frequency charts compare the **first two selected
rows in list order**; **Export selected batches** includes **all** selected batches. Statistics describe the recorded
sample, not theoretical probability. **Use parameters for new roll** requires one selection, restores its rules to
the main window and waits for an explicit **Roll**. It never changes historical results. There is no history-deletion
UI; **Clear log** affects only the recent display.

**Probability / compare** models one roll independently of historical results. Edit formulas A/B and B's mode, then
select **Calculate / compare**. A uses the main window's mode; both use the same target/pool threshold from the main
window. Change those options in the main window before reopening comparison. **Times** does not affect this one-roll
probability distribution.

- Exact calculation uses bounded convolution/enumeration with floating-point probabilities: up to 20,000 distinct
  states, 4,000,000 units of exact work, and 200,000 enumerated outcomes for a keep-rule term.
- Exceeding those budgets switches to a fixed-seed Monte Carlo estimate with 1,000–100,000 samples according to
  per-roll workload. The method/sample count are shown. Repeat analysis is reproducible within the same implementation
  and never consumes the live dice generator or creates a historical roll.
- In simulation mode, the displayed range remains theoretical; the mean and chart are estimates. For an enabled
  target, the 95% Hoeffding bound describes its estimated success proportion, not the mean or every histogram bin
  simultaneously. Calculation runs in the background and supports cancellation.

## Quick dice, presets, groups and settings

- Quick buttons offer d4, d6, d8, d10, d12, d20 and d100. They select a one-die expression, retain Times/Mode/target/pool
  options, and wait for **Roll**.
- Enter a **Preset** name, configure the expression/times/mode/target/pool options, optionally enter a **Group**, and
  select **Save preset**. There are at most 50 presets. Names/groups allow 64 UTF-16 code units each; formulas allow
  1,024. Names are globally unique ignoring case/surrounding spaces; groups do not create separate name scopes.
- Choose a group to filter the preset list. Empty Group means **all presets** when filtering and an ungrouped preset
  when saving. Choose a preset and **Use** to restore its rules; the roll name becomes its preset name. History is unchanged.
- Replacing an existing name and **Delete preset** require confirmation. Deletion affects only the preset.
- **Export presets** writes all presets, groups and rules to an offline UTF-8 `.drpresets` file. **Import presets**
  validates the file and previews new/conflicting names and resulting counts. Choose keep existing duplicates, rename
  incoming duplicates with unique suffixes, or replace existing duplicates. A merge exceeding 50 entries is rejected
  before changes are applied; cancelling the preview changes nothing.
- Preset edits/imports persist immediately. Normal exit remembers valid inputs/rule options, CSV path, automatic CSV
  export, timestamp display, normal window position/size and maximized state. Invalid unfinished inputs retain the
  last valid roll configuration; an empty CSV path can be remembered.
- Preferences/presets live in `%LOCALAPPDATA%\DiceRoller\settings.bin`. Settings version 2 reads version 1 files,
  supplying defaults for target/pool/group fields; the next successful save writes version 2. The recognized old
  default `dice_results_v2.csv` destination switches to the v3 filename without modifying the old CSV.
- Missing settings use defaults. Damaged/unreadable/unsupported settings show a warning and use defaults. Before
  replacing such a file, its original bytes are preserved in a uniquely named sibling backup; failed backup prevents
  replacement. Atomic writes preserve the old file on failure. If exit cannot save settings, you may stay or close
  using previous settings. Generated results belong to History, not the settings file.

## Local recovery and CSV export

History uses individual versioned `.dhist` records in `%LOCALAPPDATA%\DiceRoller\History`. Stable batch IDs, rules,
complete per-die results, requested/completed counts, UTC creation times, CSV status and the last recorded export
destination survive restart. Loading validates results against their rules without generating new randomness. A
corrupt record is isolated with a warning and kept untouched; other valid records can still load.

Unchecking **Automatically append to CSV** disables CSV export only: **local history is still written**. At completed-roll
boundaries the worker checkpoints when roughly 500 ms have elapsed since the last checkpoint, plus a final checkpoint
at completion or handled cancellation/error. Disk operations and roll duration can delay this interval. After an
abnormal exit, recovery restores the **last committed checkpoint**, not results computed since then or an unfinished
roll's dice. Local write failures are reported; memory-only results are not guaranteed to survive a forced exit.

You may start another batch while earlier batches await export. **Export pending** exports all pending completed
batches to the main window's CSV path. History's **Export selected batches** exports a chosen subset. Closing an
active operation asks to cancel/close and waits for safe completion/checkpoint handling. If local recovery cannot
be saved on exit, the app warns before offering to close without that recovery copy.

The default destination is `%LOCALAPPDATA%\DiceRoller\Results\dice_results_v3.csv`. Relative paths resolve against that
Results directory, independently of the working directory. Missing extensions receive `.csv`; other extensions are
rejected. Parent directories are created when saving. **Browse** works with a blank/invalid path, starts from an
existing directory and keeps the old text if cancelled.

Export persists every selected batch locally before staging a complete updated CSV beside its destination. Writes
are checked and flushed before replacement. Only after successful CSV commit are the batches' exported states saved
to History. If the process stops between commit and marking, History may still say pending while the CSV already
contains the results. Retry recognizes an identical complete batch ID and skips it instead of duplicating it.
Conflicting content or incomplete result rows for an existing ID are rejected. Export status is separate from local
history persistence and does not prevent exporting a batch to another file.

Write/flush/commit failure or cancellation before commit preserves the original CSV. Completed results remain for
retry without rerolling. Cancel arriving after commit cannot undo that successful export. If export-state persistence
fails, retry remains safe because deduplication examines the CSV itself. Normal failure/cancellation removes the
operation's temporary files and lock; pre-existing `.lock` companions are never overwritten/deleted. A force-killed
process can leave unused `.tmp` staging files, which are not recovery records. Replacement needs temporary free
space approximately equal to the old CSV plus appended rows. Do not edit/replace the destination externally during export.

New CSV v3 files use UTF-8 with a BOM, CRLF record separators and quoted/escaped fields:

```text
BatchId,ResultIndex,BatchResultCount,BatchCreatedUtc,Name,DiceNotation,Mode,RequestedRolls,Modifier,PoolThreshold,TargetEnabled,TargetComparison,TargetValue,TargetMet,Roll1,Roll2,ChosenRoll,FinalResult,Timestamp,Roll1Dice,Roll1Sides,Roll1Kept,Roll2Dice,Roll2Sides,Roll2Kept
```

`BatchResultCount` counts actually completed results, including cancelled partial batches; `RequestedRolls` retains
the original request. `BatchCreatedUtc` is UTC; `Timestamp` is the roll's local timestamp. Dice/sides/kept flags are
parallel semicolon-separated lists preserving generation order. Normal mode leaves second-group fields empty.
`ChosenRoll` precedes the modifier; `TargetMet` is blank when the target is disabled.

The v3 reader checks the header, complete batch IDs, counts and sequential indices. It accepts CRLF or LF separators
but **requires a final newline**: a missing last record terminator is treated as incomplete and rejected, not repaired.
Editing/reordering rows or changing their canonical formatting can also cause retry validation to reject a file.
Choose a new destination rather than append to a partial or altered export.

Older v2 CSVs, earlier `BaseRoll,AltRoll` files and unrelated files are preserved and rejected as v3 destinations.
They are **not imported into local History or silently converted**. The legacy v2 append routine remains for internal
callers/tests; the application's current export workflow uses v3.

### Storage limits

- Each history record: **32 MiB**. One startup load: at most **10,000 history files** and **128 MiB** of serialized
  record data. Oversized/unreadable/excess records produce warnings and remain on disk. These are load bounds,
  not automatic deletion or history-retention limits.
- Combined CSV, including existing content: **512 MiB**. Choose a new file or fewer batches when reached;
  an oversized existing CSV remains unchanged.
- Settings/preset-exchange files: **256 KiB** each. Preset files contain at most 50 entries.
- Recent display: **500 entries / 120,000 UTF-16 code units**, with a footer reporting removed older entries.
  Each recent group shows at most 30 dice; History and CSV retain all generated dice.

## Window and keyboard

- Win32 windows scale fonts/layout for per-monitor DPI. Main-window placement/DPI transitions fit the current
  monitor work area; an unavailable saved monitor falls back to the nearest available one.
- Small/high-scale windows scroll rather than shrink text. Main actions use two rows. Scrollbars, mouse wheel
  and focus-visibility handling keep controls reachable.
- Tab/Shift+Tab navigate; arrows select roll mode; Enter invokes the current default action. Alt+X exports pending batches. F1 opens About;
  File > Exit closes the application. Buttons/labels expose their underlined access keys.
- **Copy log** copies only the retained recent display. **Clear log** leaves History, pending exports and CSV files intact.

## Code structure

- `DiceCore.h/.cpp`: mixed/keep/pool rules, parsing, limits, checked arithmetic, structured results and injectable dice values.
- `DiceAnalysis.h/.cpp`: target comparisons/margins, sample statistics and bounded exact/sampled probability analysis.
- `HistoryStore.h/.cpp`: durable records, validated recovery, queries and atomic/idempotent v3 combined export.
- `ResultStore.h/.cpp`: Unicode, readable results, bounded recent log, stable paths and legacy v2 append.
- `RollJob.h/.cpp`: background work, cancellation, checkpoints, combined export and saved-state updates.
- `AppSettings.h/.cpp`: validated preferences/presets, v1 migration, backups and atomic replacement.
- `PresetExchange.h/.cpp`: `.drpresets` exchange and group/rule-aware duplicate previews/merge strategies.
- `FeatureWindows.h/.cpp`: history/details/statistics, distribution chart, probability comparison and import preview UI.
- `WindowLayout.h/.cpp`: screen-coordinate placement and DPI-aware minimum sizes bounded by the work area.
- `main.cpp`: primary controls/layout, rule/preset input, remembered state and worker integration.
- `DiceRoller.rc`, `Resource.h`, `DiceRoller.manifest`: menu, About, icons, IDs, visual styles and DPI declaration.
- `tests/Tests.cpp`, `UiTests.cpp`, `SettingsTests.cpp`, `AnalysisTests.cpp`, `HistoryTests.cpp`, `PresetTests.cpp`,
  `FeatureWindowTests.cpp`, `WorkerHistoryTests.cpp`:
  dependency-free rules, storage, analysis, exchange and hidden-window integration regressions.

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
They cover deterministic mixed/keep/pool rules, targets, boundaries, statistics/probability, Unicode, settings migration/
corruption, preset merges, checkpoints/restart recovery, idempotent export/failure preservation, cancellation, worker
state, log limits, high-DPI scrolling, keyboard operation, browsing, close-confirmation timing and continuous-batch/
history integration. Hidden-window and simulated DPI checks do not replace visual checks on actual multi-monitor hardware.
