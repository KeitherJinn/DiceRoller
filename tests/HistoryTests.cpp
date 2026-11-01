#include "HistoryStore.h"
#include "ResultStore.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace {
namespace fs = std::filesystem;
using namespace dice;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void Throws(F action) {
    bool failed = false; try { action(); } catch (const std::exception&) { failed = true; }
    Require(failed, "Expected history/export operation to fail.");
}
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); Require(static_cast<bool>(input), "Could not open history test fixture.");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Put(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc); output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close(); Require(!output.fail(), "Could not write history test fixture.");
}
struct Temporary {
    fs::path parent = fs::temp_directory_path(), root;
    Temporary() {
        GUID id{}; Require(SUCCEEDED(CoCreateGuid(&id)), "Could not create history test ID.");
        wchar_t text[40]{}; StringFromGUID2(id, text, 40);
        root = parent / (std::wstring(L"DiceRollerHistoryTests-") + text);
        Require(fs::create_directory(root), "History test directory already exists.");
    }
    ~Temporary() {
        if (root.parent_path() == parent && root.filename().wstring().find(L"DiceRollerHistoryTests-{") == 0) {
            std::error_code ignored; fs::remove_all(root, ignored);
        }
    }
};
Batch Fixture(int results = 2) {
    Batch batch;
    batch.request.name = L"中文 \"攻击\",😀";
    batch.request.notation = L"4d6kh3+1d4";
    batch.request.spec = Parse(batch.request.notation);
    batch.request.spec.successThreshold = 3;
    batch.request.mode = Mode::Advantage;
    batch.request.times = 3;
    batch.request.showTimestamp = true;
    batch.request.target = {true, Compare::AtLeast, 4};
    for (int i = 0; i < results; ++i) {
        int next = i;
        auto result = Roll(batch.request.spec, batch.request.mode, [&](int sides) { return (++next % sides) + 1; });
        result.timestamp = "2026-10-28 12:34:56";
        batch.results.push_back(std::move(result));
    }
    return batch;
}
fs::path RecordPath(const fs::path& root, const HistoryEntry& entry) { return root / (Wide(entry.id) + L".dhist"); }
void NoTemporaryFiles(const fs::path& directory) {
    for (const auto& item : fs::directory_iterator(directory))
        Require(item.path().extension() != L".tmp" && item.path().extension() != L".lock", "A failed operation left a temporary or lock file.");
}
}

void HistoryTests() {
    Temporary temporary;
    const auto root = temporary.root / L"History";
    auto entry = NewHistoryEntry(Fixture(0));
    entry.created = "2026-10-27T12:34:56.123Z";
    SaveHistoryEntry(root, entry);
    auto loaded = LoadHistory(root);
    Require(loaded.warnings.empty() && loaded.entries.size() == 1 && loaded.entries[0].batch.results.empty(),
        "An empty initial checkpoint must restore without generating results.");

    entry.batch = Fixture(2);
    SaveHistoryEntry(root, entry);
    loaded = LoadHistory(root);
    Require(loaded.warnings.empty() && loaded.entries.size() == 1 && !loaded.entries[0].saved && loaded.entries[0].batch.results.size() == 2,
        "Partial unsaved history must restore after a restart.");
    const auto& restored = loaded.entries.front();
    Require(restored.id == entry.id && restored.created == entry.created && restored.batch.request.name == entry.batch.request.name &&
        restored.batch.request.spec.terms.size() == 2 && restored.batch.request.spec.successThreshold == 3 &&
        restored.batch.request.target.enabled && restored.batch.request.target.value == 4,
        "History lost Unicode names, term rules, pool thresholds or target rules.");
    for (std::size_t i = 0; i < entry.batch.results.size(); ++i)
        Require(HistoryCsvRow(restored, i) == HistoryCsvRow(entry, i), "History per-die values/kept/sides failed to round-trip.");
    entry.batch = Fixture(3);
    SaveHistoryEntry(root, entry);
    const auto original = Read(RecordPath(root, entry));
    auto changed = entry;
    changed.batch.results.front().finalValue++;
    Throws([&] { SaveHistoryEntry(root, changed); });
    changed = entry; changed.batch.results.front().timestamp = "2026-10-28 12:34:57";
    Throws([&] { SaveHistoryEntry(root, changed); });
    changed = entry; changed.batch.results.pop_back();
    Throws([&] { SaveHistoryEntry(root, changed); });
    changed = entry; changed.batch.request.name = L"Another name";
    Throws([&] { SaveHistoryEntry(root, changed); });
    Require(Read(RecordPath(root, entry)) == original, "A checkpoint must not replace different or truncated results.");
    const auto record = RecordPath(root, entry);
    const auto recordAttributes = GetFileAttributesW(record.c_str());
    Require(recordAttributes != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(record.c_str(), recordAttributes | FILE_ATTRIBUTE_READONLY),
        "Could not make recovery snapshot read-only.");
    changed = entry; changed.saved = true; changed.exportedPath = (temporary.root / L"later.csv").wstring();
    bool snapshotFailed = false; try { SaveHistoryEntry(root, changed); } catch (const std::exception&) { snapshotFailed = true; }
    const bool snapshotAttributesRestored = SetFileAttributesW(record.c_str(), recordAttributes) != FALSE;
    Require(snapshotAttributesRestored && snapshotFailed && Read(record) == original,
        "A failed checkpoint replacement must preserve the earlier recovery snapshot.");
    NoTemporaryFiles(root);

    auto second = NewHistoryEntry(Fixture(1));
    second.created = "2026-10-28T00:00:00.000Z";
    second.batch.request.name = L"Heal";
    SaveHistoryEntry(root, second);
    loaded = LoadHistory(root);
    Require(loaded.entries.size() == 2 && loaded.warnings.empty(), "Two independent batches were not restored.");
    HistoryQuery query;
    query.name = L"攻击"; query.notation = L"D6KH3"; query.dateFrom = "2026-10-27"; query.dateTo = "2026-10-27";
    const auto matches = QueryHistory(loaded.entries, query);
    Require(matches.size() == 1 && loaded.entries[matches[0]].id == entry.id, "Unicode/name/notation/date filters failed.");
    query.dateFrom = "2026-02-30";
    Throws([&] { QueryHistory(loaded.entries, query); });

    const auto corruptRoot = temporary.root / L"Corrupt";
    fs::create_directory(corruptRoot);
    const auto corruptPath = RecordPath(corruptRoot, entry);
    for (const auto length : {std::size_t(0), std::size_t(8), std::size_t(12), original.size() / 2, original.size() - 1}) {
        const auto truncated = original.substr(0, length); Put(corruptPath, truncated);
        const auto damaged = LoadHistory(corruptRoot);
        Require(damaged.entries.empty() && damaged.warnings.size() == 1 && Read(corruptPath) == truncated,
            "Truncated history must be isolated with a warning and kept untouched.");
        Throws([&] { SaveHistoryEntry(corruptRoot, entry); });
        Require(Read(corruptPath) == truncated, "Saving must not overwrite a corrupt existing record.");
    }
    auto invalid = original; invalid[8] = 2; Put(corruptPath, invalid);
    Require(LoadHistory(corruptRoot).warnings.size() == 1, "Unsupported history version must be rejected.");
    invalid = original; invalid[12] = static_cast<char>(0xff); invalid[13] = static_cast<char>(0xff);
    invalid[14] = static_cast<char>(0xff); invalid[15] = static_cast<char>(0xff); Put(corruptPath, invalid);
    Require(LoadHistory(corruptRoot).warnings.size() == 1, "Oversized history field must be rejected before allocation.");
    {
        std::ofstream output(corruptPath, std::ios::binary | std::ios::trunc);
        output.seekp(32 * 1024 * 1024); output.put('x'); output.close();
        Require(!output.fail(), "Could not create oversized history fixture.");
    }
    Require(LoadHistory(corruptRoot).warnings.size() == 1, "Oversized history file must be rejected before allocation.");
    Put(corruptPath, "damaged");
    SaveHistoryEntry(corruptRoot, second);
    const auto isolated = LoadHistory(corruptRoot);
    Require(isolated.entries.size() == 1 && isolated.entries[0].id == second.id && isolated.warnings.size() == 1,
        "One corrupt record must not prevent valid history recovery.");

    const auto csv = temporary.root / L"合并😀.csv";
    const auto firstExport = ExportHistoryCsv(csv, {entry, second});
    Require(firstExport.appendedBatches == 2 && firstExport.appendedResults == 4 && firstExport.skippedBatches == 0,
        "Combined export did not append all selected batches.");
    const auto csvBytes = Read(csv);
    Require(csvBytes.find(HistoryCsvHeader()) == 3 && csvBytes.find(HistoryCsvRow(entry, 0)) != std::string::npos &&
        csvBytes.find(HistoryCsvRow(second, 0)) != std::string::npos, "Combined UTF-8 export is missing headers or complete details.");
    const auto repeated = ExportHistoryCsv(csv, {entry, second});
    Require(repeated.appendedResults == 0 && repeated.skippedBatches == 2 && Read(csv) == csvBytes,
        "Retry after CSV commit but before history marking must not duplicate results.");
    entry.saved = true; entry.exportedPath = csv.wstring(); SaveHistoryEntry(root, entry);
    changed = entry; changed.saved = false; changed.exportedPath.clear();
    const auto mergedState = SaveHistoryEntry(root, changed);
    Require(mergedState.saved && mergedState.exportedPath == csv.wstring(),
        "An unchanged stale pending snapshot must preserve the newer exported state.");
    loaded = LoadHistory(root); query = {}; query.unsavedOnly = true;
    const auto unsaved = QueryHistory(loaded.entries, query);
    Require(unsaved.size() == 1 && loaded.entries[unsaved[0]].id == second.id,
        "Persisted export status must distinguish saved history from recovery candidates.");
    const auto saved = std::find_if(loaded.entries.begin(), loaded.entries.end(), [&](const HistoryEntry& item) { return item.id == entry.id; });
    Require(saved != loaded.entries.end() && saved->exportedPath == csv.wstring(), "Saved destination was not restored.");

    auto third = NewHistoryEntry(Fixture(1));
    bool cancelled = false;
    try { ExportHistoryCsv(csv, {third}, [] { return true; }); } catch (const Cancelled&) { cancelled = true; }
    Require(cancelled && Read(csv) == csvBytes, "Early cancellation must preserve the existing export.");
    int checks = 0; cancelled = false;
    try { ExportHistoryCsv(csv, {third}, [&] { return ++checks >= 10; }); } catch (const Cancelled&) { cancelled = true; }
    Require(cancelled && Read(csv) == csvBytes, "Cancellation during staging must preserve the existing export.");
    NoTemporaryFiles(temporary.root);
    changed = second; changed.batch.request.name = L"Conflicting same ID";
    Throws([&] { ExportHistoryCsv(csv, {changed}); });
    Require(Read(csv) == csvBytes, "An ID/content conflict must preserve the original export.");
    const auto partialCsv = temporary.root / L"partial.csv";
    Put(partialCsv, std::string("\xEF\xBB\xBF") + HistoryCsvHeader() + "\r\n" + HistoryCsvRow(entry, 0));
    const auto partialBytes = Read(partialCsv);
    Throws([&] { ExportHistoryCsv(partialCsv, {entry}); });
    Require(Read(partialCsv) == partialBytes, "A pre-existing partial batch must not be treated as a successful export.");
    const auto legacy = temporary.root / L"legacy.csv";
    Put(legacy, CsvHeader() + "\r\n");
    const auto legacyBytes = Read(legacy);
    Throws([&] { ExportHistoryCsv(legacy, {entry}); });
    Require(Read(legacy) == legacyBytes, "Legacy CSV must remain unchanged with an actionable new-file requirement.");
    const auto attributes = GetFileAttributesW(csv.c_str());
    Require(attributes != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(csv.c_str(), attributes | FILE_ATTRIBUTE_READONLY),
        "Could not make export read-only.");
    bool failed = false; try { ExportHistoryCsv(csv, {third}); } catch (const std::exception&) { failed = true; }
    const bool restoredAttributes = SetFileAttributesW(csv.c_str(), attributes) != FALSE;
    Require(restoredAttributes && failed && Read(csv) == csvBytes, "A failed commit must preserve original CSV bytes.");
    const auto lock = CreateFileW(csv.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "Could not lock export fixture.");
    failed = false; try { ExportHistoryCsv(csv, {third}); } catch (const std::exception&) { failed = true; }
    CloseHandle(lock);
    Require(failed && Read(csv) == csvBytes, "A locked export must remain unchanged.");
    const auto retry = ExportHistoryCsv(csv, {entry, third});
    Require(retry.skippedBatches == 1 && retry.appendedBatches == 1 && retry.appendedResults == 1,
        "A combined retry must skip existing batches and append only new ones.");
    NoTemporaryFiles(temporary.root);
    Throws([&] { ExportHistoryCsv(csv, {NewHistoryEntry(Fixture(0))}); });
    Throws([&] { ExportHistoryCsv(csv, {third, third}); });

    const auto lightweight = ScanHistory(root);
    Require(lightweight.warnings.empty() && lightweight.entries.size() == 2 &&
        lightweight.entries.front().id == second.id && !lightweight.entries.front().saved,
        "The lightweight index must prioritize pending records without loading dice data.");
    HistoryQuery pendingQuery; pendingQuery.unsavedOnly = true;
    Require(QueryHistoryIndex(lightweight.entries, pendingQuery).size() == 1, "Index filtering lost pending history.");
    const auto onDemand = ReadHistoryEntry(root, entry.id);
    Require(HistoryCsvRow(onDemand, 0) == HistoryCsvRow(entry, 0), "On-demand history reading changed stored results.");
    Throws([&] { ArchiveHistoryEntry(root, second.id); });
    const auto archiveSummary = ArchiveHistoryEntry(root, entry.id);
    Require(archiveSummary.archived && archiveSummary.saved && !fs::exists(RecordPath(root, entry)) &&
        fs::exists(root / L"Archive" / (Wide(entry.id) + L".dhist")), "Archiving must atomically move an exported record.");
    Require(ScanHistory(root).entries.size() == 1 && ScanHistory(root, true).entries.size() == 2,
        "Archived records must remain accessible without filling the active index.");
    const auto archivedEntry = ReadHistoryEntry(root, entry.id, true);
    Require(archivedEntry.archived && HistoryCsvRow(archivedEntry, 0) == HistoryCsvRow(entry, 0),
        "Archived dice details failed to round-trip.");
    const auto archivedCsv = temporary.root / L"archived.csv";
    Require(ExportHistoryCsvIds(archivedCsv, root, {archiveSummary}).appendedResults == entry.batch.results.size(),
        "Archived records must support the bounded ID-based export path.");
    const auto archivedBytes = Read(root / L"Archive" / (Wide(entry.id) + L".dhist"));
    Put(RecordPath(root, entry), archivedBytes);
    Throws([&] { ArchiveHistoryEntry(root, entry.id); });
    Require(Read(RecordPath(root, entry)) == archivedBytes, "An archive filename conflict must preserve the active record.");

    bool scanCancelled = false;
    try { ScanHistory(root, true, [] { return true; }); } catch (const Cancelled&) { scanCancelled = true; }
    Require(scanCancelled, "History indexing must observe cancellation before scanning files.");
    bool readCancelled = false;
    try { ReadHistoryEntry(root, entry.id, true, [] { return true; }); } catch (const Cancelled&) { readCancelled = true; }
    Require(readCancelled, "On-demand history reads must support cancellation.");
    bool exportCancelled = false;
    try { ExportHistoryCsvIds(archivedCsv, root, {archiveSummary}, [] { return true; }); } catch (const Cancelled&) { exportCancelled = true; }
    Require(exportCancelled, "ID-based export must observe cancellation before writing.");

    // The count limit is removed from the index, including records beyond the
    // former 10,000-file cutoff. Reuse valid record bytes with unique v1 IDs.
    const auto manyRoot = temporary.root / L"Many"; fs::create_directory(manyRoot);
    auto smallEntry = NewHistoryEntry(Fixture(0)); SaveHistoryEntry(manyRoot, smallEntry);
    const auto smallBytes = Read(RecordPath(manyRoot, smallEntry));
    for (unsigned i = 0; i < 10000; ++i) {
        char idText[40]{}; std::snprintf(idText, sizeof(idText), "%08X-0000-0000-0000-000000000000", i);
        auto recordBytes = smallBytes; recordBytes.replace(16, 36, idText);
        Put(manyRoot / (Wide(idText) + L".dhist"), recordBytes);
    }
    const auto many = ScanHistory(manyRoot);
    Require(many.entries.size() == 10001 && many.warnings.empty(), "The index must not truncate at 10,000 files.");

    // Each large fixture has valid bounded metadata, but intentionally unverified
    // zero-filled result data. Indexing must inspect metadata only, regardless of
    // the aggregate payload size; a detail read must still reject the bad payload.
    const auto largeRoot = temporary.root / L"LargeMetadata"; fs::create_directory(largeRoot);
    Batch large; large.request.name = L"Large index fixture"; large.request.notation = L"1000d1";
    large.request.spec = Parse(large.request.notation); large.request.times = 1000;
    auto largeEntry = NewHistoryEntry(std::move(large)); SaveHistoryEntry(largeRoot, largeEntry);
    auto prefix = Read(RecordPath(largeRoot, largeEntry));
    for (unsigned shift = 0; shift < 32; shift += 8) prefix[prefix.size() - 4 + shift / 8] = static_cast<char>((1000u >> shift) & 0xff);
    const auto fileSize = static_cast<std::uintmax_t>(prefix.size()) + 1000ULL * ((12ULL + 1000ULL * 12ULL) + 24ULL);
    for (unsigned i = 0; i < 12; ++i) {
        char idText[40]{}; std::snprintf(idText, sizeof(idText), "%08X-1111-1111-1111-111111111111", i);
        auto recordBytes = prefix; recordBytes.replace(16, 36, idText);
        const auto largePath = largeRoot / (Wide(idText) + L".dhist"); Put(largePath, recordBytes); fs::resize_file(largePath, fileSize);
    }
    const auto largeIndex = ScanHistory(largeRoot);
    Require(largeIndex.entries.size() == 13 && largeIndex.warnings.empty(), "Metadata indexing must not stop at a 128 MiB payload budget.");
    Throws([&] { ReadHistoryEntry(largeRoot, "00000000-1111-1111-1111-111111111111"); });
}
