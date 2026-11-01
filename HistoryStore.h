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
    bool archived = false; // Physical location only; not part of the v1 record bytes.
};
struct HistorySummary {
    std::string id;
    std::string created;
    Request request;
    std::size_t resultCount = 0;
    bool saved = false;
    std::wstring exportedPath;
    bool archived = false;
    bool localPersisted = true;
};
struct HistoryIndex {
    std::vector<HistorySummary> entries;
    std::vector<std::wstring> warnings;
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
void ValidateHistoryEntry(const HistoryEntry& entry, const CancelCheck& cancelled = {});
HistorySummary SummaryOf(const HistoryEntry& entry);
// Reads bounded metadata only. Unlike legacy LoadHistory, does not truncate the
// index at a total result-data budget or record count. Details load on demand.
HistoryIndex ScanHistory(const std::filesystem::path& root, bool includeArchived = false,
    const CancelCheck& cancelled = {});
HistoryEntry ReadHistoryEntry(const std::filesystem::path& root, const std::string& id,
    bool archived = false, const CancelCheck& cancelled = {});
std::vector<std::size_t> QueryHistoryIndex(const std::vector<HistorySummary>& entries, const HistoryQuery& query);
// Only exported records can move into Archive. An existing destination is never
// replaced. The returned summary identifies the new location for subsequent reads.
HistorySummary ArchiveHistoryEntry(const std::filesystem::path& root, const std::string& id);
// A stable ID supports atomic checkpoints: only an unchanged result prefix may
// grow. Exported records are immutable. Corrupt existing records are never replaced.
HistorySummary SaveHistoryEntry(const std::filesystem::path& root, const HistoryEntry& entry,
    const CancelCheck& cancelled = {});
HistoryLoad LoadHistory(const std::filesystem::path& root);
std::vector<std::size_t> QueryHistory(const std::vector<HistoryEntry>& entries, const HistoryQuery& query);
const std::string& HistoryCsvHeader();
std::string HistoryCsvRow(const HistoryEntry& entry, std::size_t resultIndex);
// Atomic combined v3 export. Identical complete batch IDs already in this file
// are skipped. Conflicting or partial IDs and older CSV formats are rejected.
// Does not mark history saved: callers persist that only after this succeeds.
ExportSummary ExportHistoryCsv(const std::filesystem::path& path,
    const std::vector<HistoryEntry>& entries, const CancelCheck& cancelled = {});
// Reads and exports one complete record at a time; full batch data is not retained
// for the entire selection. Summary locations identify active or archived records.
ExportSummary ExportHistoryCsvIds(const std::filesystem::path& path, const std::filesystem::path& root,
    const std::vector<HistorySummary>& entries, const CancelCheck& cancelled = {});
}
