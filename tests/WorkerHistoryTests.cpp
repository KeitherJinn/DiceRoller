#include "RollJob.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>

namespace {
namespace fs = std::filesystem;
using namespace dice;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Temporary {
    fs::path parent = fs::temp_directory_path(), root;
    Temporary() {
        GUID id{}; Check(SUCCEEDED(CoCreateGuid(&id)), "Could not create worker test identity.");
        wchar_t text[40]{}; StringFromGUID2(id, text, 40);
        root = parent / (std::wstring(L"DiceWorkerHistoryTests-") + text);
        Check(fs::create_directory(root), "Worker test directory already exists.");
    }
    ~Temporary() {
        if (root.parent_path() == parent && root.filename().wstring().find(L"DiceWorkerHistoryTests-{") == 0) {
            std::error_code ignored; fs::remove_all(root, ignored);
        }
    }
};
Request RequestFixture() {
    Request request; request.name = L"Worker history"; request.notation = L"2d1+3";
    request.spec = Parse(request.notation); request.times = 2; return request;
}
HistoryEntry Fixture() {
    Batch batch; batch.request = RequestFixture();
    for (int i = 0; i < batch.request.times; ++i) {
        auto roll = Roll(batch.request.spec, batch.request.mode, [](int) { return 1; });
        roll.timestamp = "2026-10-31 12:00:00"; batch.results.push_back(std::move(roll));
    }
    return NewHistoryEntry(std::move(batch));
}
Outcome Wait(RollJob& job) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (job.State() != Phase::Finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(job.State() == Phase::Finished, "Worker history operation timed out."); return job.Take();
}
bool Contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}
struct ReadOnly {
    fs::path path; DWORD previous;
    explicit ReadOnly(fs::path value) : path(std::move(value)), previous(GetFileAttributesW(path.c_str())) {
        Check(previous != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(path.c_str(), previous | FILE_ATTRIBUTE_READONLY),
            "Could not make worker fixture read-only.");
    }
    ~ReadOnly() { SetFileAttributesW(path.c_str(), previous); }
};
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Check(static_cast<bool>(input), "Could not read worker test fixture.");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Put(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close(); Check(!output.fail(), "Could not write worker test fixture.");
}
Request AdvancedRequest(const wchar_t* notation = L"4d6kh3+d4", int times = 5) {
    Request request;
    request.name = L"worker 中文 audit"; request.notation = notation;
    request.spec = Parse(notation); request.times = times;
    request.target = {true, Compare::AtLeast, 12};
    return request;
}
HistoryEntry AdvancedFixture(int times = 3) {
    Batch batch; batch.request = AdvancedRequest(L"2d6kh1+d4", times);
    for (int i = 0; i < times; ++i) {
        int value = i;
        auto result = Roll(batch.request.spec, batch.request.mode, [&](int sides) { return ++value % sides + 1; });
        result.timestamp = "2026-10-28 12:34:56";
        batch.results.push_back(std::move(result));
    }
    return NewHistoryEntry(std::move(batch));
}
void SameResults(const HistoryEntry& expected, const HistoryEntry& actual) {
    Check(expected.id == actual.id && expected.created == actual.created && expected.batch.results.size() == actual.batch.results.size(),
        "Worker changed the checkpoint identity or result count.");
    for (std::size_t i = 0; i < expected.batch.results.size(); ++i)
        Check(HistoryCsvRow(expected, i) == HistoryCsvRow(actual, i), "Worker recovery changed exact per-die data, metadata or timestamps.");
}
std::string CsvBytes(const std::vector<HistoryEntry>& entries) {
    std::string bytes = std::string("\xEF\xBB\xBF") + HistoryCsvHeader() + "\r\n";
    for (const auto& entry : entries)
        for (std::size_t i = 0; i < entry.batch.results.size(); ++i) bytes += HistoryCsvRow(entry, i);
    return bytes;
}
void CheckPersisted(const fs::path& root, const HistoryEntry& expected) {
    const auto actual = ReadHistoryEntry(root, expected.id, expected.archived);
    SameResults(expected, actual);
    Check(actual.saved == expected.saved && actual.exportedPath == expected.exportedPath, "Worker export status did not persist.");
}
std::vector<HistorySummary> Summaries(const std::vector<HistoryEntry>& entries) {
    std::vector<HistorySummary> summaries;
    for (const auto& entry : entries) summaries.push_back(SummaryOf(entry));
    return summaries;
}
void AdvancedWorkerRegressionTests(const fs::path& base) {
    RollJob job;
    const auto root = base / L"advanced auto history", csv = base / L"advanced auto.csv";
    auto request = AdvancedRequest(); request.mode = Mode::Advantage; request.spec.successThreshold = 3;
    job.StartRoll(request, csv, root); auto outcome = Wait(job);
    Check(outcome.saved && outcome.csvCommitted && !outcome.cancelled && outcome.error.empty() && outcome.batch &&
        outcome.batch->results.size() == 5 && outcome.summaries.size() == 1 && outcome.entries.empty(),
        "Advanced worker automatic export did not complete with durable lightweight history.");
    auto automatic = ReadHistoryEntry(root, outcome.summaries.front().id);
    Check(automatic.saved && automatic.exportedPath == csv.wstring(), "Successful automatic export was not marked.");
    Check(Read(csv) == CsvBytes({automatic}), "Advanced automatic export does not contain exactly the computed results.");
    auto sameBatch = automatic; sameBatch.batch = *outcome.batch; SameResults(automatic, sameBatch); CheckPersisted(root, automatic);

    const auto failureRoot = base / L"CSV failure history", invalidCsv = base / L"older.csv";
    const std::string oldBytes = "older header\nuntouched\n"; Put(invalidCsv, oldBytes);
    job.StartRoll(AdvancedRequest(), invalidCsv, failureRoot); outcome = Wait(job);
    Check(!outcome.saved && !outcome.csvCommitted && !outcome.cancelled && !outcome.error.empty() && outcome.batch &&
        outcome.batch->results.size() == 5 && outcome.summaries.size() == 1 && outcome.dirtyEntries.empty(),
        "Failed CSV export must retain all generated results durably.");
    const auto failed = ReadHistoryEntry(failureRoot, outcome.summaries.front().id);
    Check(!failed.saved && Read(invalidCsv) == oldBytes, "Failed CSV export changed its status or the old file.");
    sameBatch = failed; sameBatch.batch = *outcome.batch; SameResults(failed, sameBatch); CheckPersisted(failureRoot, failed);
    const auto retryCsv = base / L"exact retry.csv";
    job.StartExportIds(outcome.summaries, retryCsv, failureRoot); outcome = Wait(job);
    Check(outcome.saved && outcome.error.empty() && outcome.summaries.size() == 1, "Durable exact retry failed.");
    auto retried = ReadHistoryEntry(failureRoot, failed.id); SameResults(failed, retried); CheckPersisted(failureRoot, retried);
    Check(Read(retryCsv) == CsvBytes({retried}), "Retry changed or duplicated dice results.");
    const auto retryBytes = Read(retryCsv);
    job.StartExportIds(outcome.summaries, retryCsv, failureRoot); outcome = Wait(job);
    Check(outcome.saved && outcome.error.empty() && Read(retryCsv) == retryBytes, "Repeating a successful export duplicated a batch.");

    const auto cancelRoot = base / L"cancelled rolls";
    request = AdvancedRequest(L"1000d6kh500", 1000);
    job.StartRoll(request, std::nullopt, cancelRoot);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (job.Completed() < 2 && job.State() != Phase::Finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const auto observed = job.Completed(); job.Cancel(); outcome = Wait(job);
    Check(observed >= 2 && outcome.error.empty() && !outcome.saved && outcome.batch && outcome.batch->results.size() >= observed &&
        outcome.summaries.size() == 1 && outcome.localPersisted && outcome.dirtyEntries.empty(),
        "Cancellation lost a completed roll or failed to checkpoint it.");
    Check(outcome.cancelled || outcome.batch->results.size() == 1000, "Early cancellation was not reported.");
    const auto cancelledEntry = ReadHistoryEntry(cancelRoot, outcome.summaries.front().id);
    sameBatch = cancelledEntry; sameBatch.batch = *outcome.batch; SameResults(cancelledEntry, sameBatch); CheckPersisted(cancelRoot, cancelledEntry);
    for (const auto& result : outcome.batch->results)
        Check(result.first.dice.size() == 1000 && result.first.sides.size() == 1000 && result.first.kept.size() == 1000 &&
            std::count(result.first.kept.begin(), result.first.kept.end(), true) == 500, "Cancelled batch contains a partial dice group.");
    const auto cancelledCsv = base / L"cancelled rolls.csv";
    job.StartExportIds(outcome.summaries, cancelledCsv, cancelRoot); outcome = Wait(job);
    Check(outcome.saved && outcome.error.empty(), "Cancelled partial batch could not be exported.");
    SameResults(cancelledEntry, ReadHistoryEntry(cancelRoot, cancelledEntry.id));
    Check(Read(cancelledCsv) == CsvBytes({cancelledEntry}), "Export changed completed rolls retained after cancellation.");

    const auto exportRoot = base / L"cancel export history", cancelCsv = base / L"cancel export.csv";
    const auto baseline = AdvancedFixture(2); SaveHistoryEntry(exportRoot, baseline);
    ExportHistoryCsv(cancelCsv, {baseline}); const auto originalCsv = Read(cancelCsv);
    std::vector<HistoryEntry> entries{AdvancedFixture(300), AdvancedFixture(300)};
    for (const auto& entry : entries) SaveHistoryEntry(exportRoot, entry);
    job.StartExportIds(Summaries(entries), cancelCsv, exportRoot); job.Cancel(); outcome = Wait(job);
    Check(outcome.error.empty() && (outcome.cancelled || outcome.saved) && outcome.summaries.size() == entries.size(),
        "Export cancellation returned an inconsistent outcome.");
    for (const auto& entry : entries) SameResults(entry, ReadHistoryEntry(exportRoot, entry.id));
    if (!outcome.csvCommitted) {
        Check(outcome.cancelled && !outcome.saved && Read(cancelCsv) == originalCsv, "Pre-commit cancellation changed the existing CSV.");
        for (const auto& entry : entries) Check(!ReadHistoryEntry(exportRoot, entry.id).saved, "Cancelled uncommitted export marked a batch saved.");
    } else Check(Read(cancelCsv) == CsvBytes({baseline, entries[0], entries[1]}), "Late export cancellation left incomplete CSV data.");

    // Observe the committed file before cancelling status updates. A scheduler
    // may finish all marks first; either outcome must preserve complete rows.
    const auto lateRoot = base / L"late cancel history", lateCsv = base / L"late cancel.csv";
    entries.clear();
    for (int i = 0; i < 16; ++i) { entries.push_back(AdvancedFixture(300)); SaveHistoryEntry(lateRoot, entries.back()); }
    job.StartExportIds(Summaries(entries), lateCsv, lateRoot);
    const auto commitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!fs::exists(lateCsv) && job.State() != Phase::Finished && std::chrono::steady_clock::now() < commitDeadline)
        std::this_thread::yield();
    Check(fs::exists(lateCsv), "Export did not reach the commit point in time.");
    job.Cancel(); outcome = Wait(job);
    Check(outcome.csvCommitted && outcome.error.empty() && (outcome.saved || outcome.cancelled) && Read(lateCsv) == CsvBytes(entries),
        "Post-commit cancellation changed committed results or reported an uncommitted export.");
    Check(outcome.dirtyEntries.empty() && outcome.localPersisted, "Export cancellation made durable dice results dirty.");
    for (const auto& entry : entries) {
        const auto disk = ReadHistoryEntry(lateRoot, entry.id); SameResults(entry, disk);
        Check(disk.saved || std::any_of(outcome.pendingMarks.begin(), outcome.pendingMarks.end(), [&](const auto& mark) { return mark.id == entry.id; }),
            "Post-commit cancellation lost a remaining saved-status update.");
    }
    job.StartFlushDirty({}, lateRoot, outcome.pendingMarks); outcome = Wait(job);
    Check(outcome.saved && outcome.error.empty() && Read(lateCsv) == CsvBytes(entries), "Post-commit status recovery changed CSV data.");
    for (const auto& entry : entries) Check(ReadHistoryEntry(lateRoot, entry.id).saved, "Post-commit status recovery left an unmarked batch.");

    // Crash gap: CSV contains both IDs, but only one saved mark reached disk.
    const auto markRoot = base / L"mixed marks", markedCsv = base / L"mixed marks.csv";
    entries = {AdvancedFixture(2), AdvancedFixture(3)};
    for (const auto& entry : entries) SaveHistoryEntry(markRoot, entry);
    ExportHistoryCsv(markedCsv, entries); const auto committedBytes = Read(markedCsv);
    entries[0].saved = true; entries[0].exportedPath = markedCsv.wstring(); SaveHistoryEntry(markRoot, entries[0]);
    {
        ReadOnly readonly(markRoot / (Wide(entries[1].id) + L".dhist"));
        job.StartExportIds(Summaries(entries), markedCsv, markRoot); outcome = Wait(job);
        Check(outcome.csvCommitted && !outcome.saved && !outcome.error.empty() && outcome.pendingMarks.size() == 1 &&
            outcome.pendingMarks.front().id == entries[1].id && outcome.dirtyEntries.empty() &&
            ReadHistoryEntry(markRoot, entries[0].id).saved && !ReadHistoryEntry(markRoot, entries[1].id).saved,
            "Partial status failure did not preserve each batch's durable export state.");
        Check(Read(markedCsv) == committedBytes, "A local history failure changed the committed CSV.");
    }
    job.StartExportIds(outcome.summaries, markedCsv, markRoot); outcome = Wait(job);
    Check(outcome.saved && outcome.error.empty() && outcome.pendingMarks.empty() && Read(markedCsv) == committedBytes,
        "Recovery after partial marking duplicated committed CSV rows.");
    for (const auto& entry : entries) {
        const auto disk = ReadHistoryEntry(markRoot, entry.id); SameResults(entry, disk);
        Check(disk.saved && disk.exportedPath == markedCsv.wstring(), "Recovered mixed export was not marked saved.");
    }

    RollJob invalidJob;
    for (const auto control : {wchar_t(0), wchar_t(9), wchar_t(31), wchar_t(127)}) {
        auto invalid = AdvancedRequest(); invalid.name += control; bool rejected = false;
        try { invalidJob.StartRoll(invalid, std::nullopt, base / L"invalid metadata"); } catch (const std::exception&) { rejected = true; }
        Check(rejected && invalidJob.State() == Phase::Idle && invalidJob.Completed() == 0, "Control metadata must be rejected before generating dice.");
        invalid = AdvancedRequest(); invalid.notation += control; rejected = false;
        try { invalidJob.StartRoll(invalid, std::nullopt, base / L"invalid metadata"); } catch (const std::exception&) { rejected = true; }
        Check(rejected && invalidJob.State() == Phase::Idle && invalidJob.Completed() == 0, "Control characters in notation must be rejected before generating dice.");
    }
    Check(!fs::exists(base / L"invalid metadata"), "Invalid metadata created a history checkpoint.");
}
}

void WorkerHistoryTests() {
    Temporary temporary; const auto root = temporary.root / L"History"; RollJob job;
    job.StartRoll(RequestFixture(), std::nullopt, root);
    auto rolled = Wait(job);
    Check(rolled.batch && rolled.batch->results.size() == 2 && rolled.summaries.size() == 1 && rolled.entries.empty() &&
        rolled.dirtyEntries.empty() && rolled.localPersisted && Contains(rolled.persistedIds, rolled.summaries.front().id),
        "Successful rolling must return lightweight durable summaries, not duplicate all results in history entries.");
    const auto id = rolled.summaries.front().id;
    Check(ReadHistoryEntry(root, id).batch.results.size() == 2, "Completed rolls were not durably checkpointed.");

    const auto csv = temporary.root / L"export.csv";
    job.StartExportIds(rolled.summaries, csv, root); auto exported = Wait(job);
    Check(exported.saved && exported.csvCommitted && exported.localPersisted && exported.entries.empty() &&
        exported.dirtyEntries.empty() && exported.pendingMarks.empty() && exported.summaries.front().saved,
        "ID-based export must return persisted metadata without copying every full batch.");
    // Simulate a second caller retaining the old pending flag. The old record is
    // already durable/exported; an identical read-only snapshot must need no write.
    const auto recordPath = root / (Wide(id) + L".dhist");
    {
        ReadOnly readonly(recordPath);
        job.StartExportIds(rolled.summaries, csv, root); auto retried = Wait(job);
        Check(retried.saved && retried.csvCommitted && retried.pendingMarks.empty() && retried.summaries.front().saved,
            "A stale pending summary must not block an identical already-exported read-only record.");
    }

    const auto pending = Fixture(); SaveHistoryEntry(root, pending);
    const auto pendingPath = root / (Wide(pending.id) + L".dhist");
    Outcome markFailed;
    {
        ReadOnly readonly(pendingPath);
        job.StartExportIds({SummaryOf(pending)}, temporary.root / L"mark-failure.csv", root);
        markFailed = Wait(job);
        Check(markFailed.csvCommitted && !markFailed.saved && markFailed.pendingMarks.size() == 1 &&
            markFailed.dirtyEntries.empty() && markFailed.localPersisted && !markFailed.error.empty(),
            "CSV commit followed by a metadata failure must retain only a lightweight pending mark, not a full dirty batch.");
        Check(!ReadHistoryEntry(root, pending.id).saved, "A failed export mark unexpectedly changed disk status.");
    }
    job.StartFlushDirty({}, root, markFailed.pendingMarks); auto marked = Wait(job);
    Check(marked.saved && !marked.csvCommitted && marked.pendingMarks.empty() && Contains(marked.persistedIds, pending.id) &&
        ReadHistoryEntry(root, pending.id).saved, "Background metadata repair must finish without exporting or rerolling again.");

    const auto blocked = temporary.root / L"blocked";
    { std::ofstream file(blocked); file << "not a directory"; }
    job.StartRoll(RequestFixture(), std::nullopt, blocked); auto failed = Wait(job);
    Check(!failed.localPersisted && failed.dirtyEntries.size() == 1 && failed.batch && failed.batch->results.size() == 2 &&
        failed.summaries.size() == 1 && !failed.summaries.front().localPersisted,
        "Only genuinely unpersisted results should remain as a complete dirty entry.");
    const auto dirtyId = failed.dirtyEntries.front().id;
    job.StartFlushDirty(failed.dirtyEntries, root); auto flushed = Wait(job);
    Check(flushed.saved && flushed.localPersisted && flushed.dirtyEntries.empty() && Contains(flushed.persistedIds, dirtyId),
        "Closing-time background flushing must report exactly the recovered IDs.");
    Check(ReadHistoryEntry(root, dirtyId).batch.results.size() == 2, "Dirty retry lost completed rolls.");

    auto fresh = Fixture(); auto summary = SummaryOf(fresh); summary.localPersisted = false;
    job.StartExportIds({summary}, temporary.root / L"dirty-export.csv", root, {fresh}); auto combined = Wait(job);
    Check(combined.saved && combined.localPersisted && combined.csvCommitted && combined.dirtyEntries.empty() &&
        Contains(combined.persistedIds, fresh.id) && combined.summaries.front().saved,
        "Exporting a selected dirty batch must persist it first and then return its actual saved summary.");

    // A failed selected dirty entry must not prevent other dirty records from being
    // saved, and must not make unrelated clean historical batches dirty again.
    auto good = Fixture(), bad = Fixture(); SaveHistoryEntry(root, bad);
    bad.batch.results.front().timestamp = "2026-10-31 12:00:01";
    job.StartFlushDirty({bad, good}, root); auto partlyFlushed = Wait(job);
    Check(!partlyFlushed.saved && partlyFlushed.dirtyEntries.size() == 1 && partlyFlushed.dirtyEntries.front().id == bad.id &&
        Contains(partlyFlushed.persistedIds, good.id) && !Contains(partlyFlushed.persistedIds, bad.id),
        "A partial flush must identify successful IDs and retain only records still needing recovery.");

    const auto archived = ArchiveHistoryEntry(root, id);
    job.StartExportIds({archived}, temporary.root / L"archived-export.csv", root); auto archiveExport = Wait(job);
    Check(archiveExport.saved && archiveExport.summaries.front().archived && ReadHistoryEntry(root, id, true).saved,
        "ID-based export must load archived records from their actual location.");
    AdvancedWorkerRegressionTests(temporary.root);
}
