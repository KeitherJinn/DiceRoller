#pragma once
#include "DiceCore.h"
#include <filesystem>

namespace dice {
struct HistoryEntry {
    std::string id;
    std::string created; // UTC ISO 8601, YYYY-MM-DDTHH:MM:SS.mmmZ.
    Batch batch;
    bool saved = false;
    std::wstring exportedPath;
};
struct HistoryLoad {
    std::vector<HistoryEntry> entries;
    std::vector<std::wstring> warnings;
};
struct HistoryQuery {
    std::wstring name;
    std::wstring notation;
    std::string dateFrom; // Inclusive UTC dates, YYYY-MM-DD; empty is unbounded.
    std::string dateTo;
    bool unsavedOnly = false;
};
struct ExportSummary {
    std::size_t appendedBatches = 0;
    std::size_t skippedBatches = 0;
    std::size_t appendedResults = 0;
};

std::filesystem::path DefaultHistoryDirectory();
HistoryEntry NewHistoryEntry(Batch batch);
void ValidateHistoryEntry(const HistoryEntry& entry);
// A stable ID supports atomic checkpoints: only an unchanged result prefix may
// grow. Exported records are immutable. Corrupt existing records are never replaced.
void SaveHistoryEntry(const std::filesystem::path& root, const HistoryEntry& entry);
HistoryLoad LoadHistory(const std::filesystem::path& root);
std::vector<std::size_t> QueryHistory(const std::vector<HistoryEntry>& entries, const HistoryQuery& query);
const std::string& HistoryCsvHeader();
std::string HistoryCsvRow(const HistoryEntry& entry, std::size_t resultIndex);
// Atomic combined v3 export. Identical complete batch IDs already in this file
// are skipped. Conflicting or partial IDs and older CSV formats are rejected.
// Does not mark history saved: callers persist that only after this succeeds.
ExportSummary ExportHistoryCsv(const std::filesystem::path& path,
    const std::vector<HistoryEntry>& entries, const CancelCheck& cancelled = {});
}
