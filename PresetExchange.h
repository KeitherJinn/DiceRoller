#pragma once

#include "AppSettings.h"

namespace dice {
enum class ImportStrategy { KeepExisting, RenameIncoming, ReplaceExisting };
struct ImportPreview {
    std::size_t existingCount = 0;
    std::size_t incomingCount = 0;
    std::size_t newCount = 0;
    std::size_t conflictCount = 0;
    std::size_t resultCount = 0;        // KeepExisting or ReplaceExisting.
    std::size_t renamedResultCount = 0;
    std::vector<std::wstring> conflicts;
};

// Offline UTF-8 .drpresets text: "DiceRollerPresets\t1", then nine tab-separated
// fields per record: name, group, notation, times, mode, success threshold,
// target enabled, target comparison, target value. CRLF output; LF input accepted.
// All user text is single-line and cannot contain tabs. Export replaces atomically.
void ExportPresets(const std::filesystem::path& path, const std::vector<Preset>& presets);
std::vector<Preset> ReadPresets(const std::filesystem::path& path);

// Names are globally unique ignoring case and surrounding whitespace; groups do
// not create a separate name scope. Incoming duplicates are processed in order:
// KeepExisting keeps the first, ReplaceExisting keeps the last, RenameIncoming
// retains all with unique " (2)" suffixes. Neither function mutates its arguments.
// Preview permits a combined count above 50 so the UI can explain each strategy;
// Merge rejects any result above MaxPresets before returning any changes.
ImportPreview PreviewImport(const std::vector<Preset>& current, const std::vector<Preset>& incoming);
std::vector<Preset> MergePresets(const std::vector<Preset>& current,
    const std::vector<Preset>& incoming, ImportStrategy strategy);
}
