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
        GUID id{}; Check(SUCCEEDED(CoCreateGuid(&id)), "Could not create worker test ID.");
        wchar_t text[40]{}; StringFromGUID2(id, text, 40);
        root = parent / (std::wstring(L"DiceRollerWorkerHistoryTests-") + text);
        Check(fs::create_directory(root), "Worker test directory already exists.");
    }
    ~Temporary() {
        if (root.parent_path() == parent && root.filename().wstring().find(L"DiceRollerWorkerHistoryTests-{") == 0) {
            std::error_code ignored; fs::remove_all(root, ignored);
        }
    }
};
struct ReadOnly {
    fs::path path;
    DWORD original;
    explicit ReadOnly(fs::path file) : path(std::move(file)), original(GetFileAttributesW(path.c_str())) {
        Check(original != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(path.c_str(), original | FILE_ATTRIBUTE_READONLY), "Could not make worker history fixture read-only.");
    }
    ~ReadOnly() { SetFileAttributesW(path.c_str(), original); }
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
Request MakeRequest(const wchar_t* notation = L"4d6kh3+d4", int times = 5) {
    Request request;
    request.name = L"worker 中文 audit";
    request.notation = notation;
    request.spec = Parse(notation);
    request.times = times;
    request.target = {true, Compare::AtLeast, 12};
    return request;
}
HistoryEntry Fixture(int times = 3) {
    Batch batch;
    batch.request = MakeRequest(L"2d6kh1+d4", times);
    for (int i = 0; i < times; ++i) {
        int value = i;
        auto result = Roll(batch.request.spec, batch.request.mode, [&](int sides) { return ++value % sides + 1; });
        result.timestamp = "2026-10-28 12:34:56";
        batch.results.push_back(std::move(result));
    }
    return NewHistoryEntry(std::move(batch));
}
Outcome Wait(RollJob& job) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (job.State() != Phase::Finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (job.State() != Phase::Finished) { job.Cancel(); throw std::runtime_error("History worker did not finish in time."); }
    return job.Take();
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
    const auto loaded = LoadHistory(root);
    Check(loaded.warnings.empty(), "Worker history failed validation on restart.");
    const auto found = std::find_if(loaded.entries.begin(), loaded.entries.end(), [&](const auto& value) { return value.id == expected.id; });
    Check(found != loaded.entries.end(), "Worker history entry was not durable.");
    SameResults(expected, *found);
    Check(found->saved == expected.saved && found->exportedPath == expected.exportedPath, "Worker export status did not persist.");
}
}

void WorkerHistoryTests() {
    using namespace dice;
    Temporary temporary;
    RollJob job;
    const auto root = temporary.root / L"auto history";
    const auto csv = temporary.root / L"auto.csv";
    auto request = MakeRequest();
    request.mode = Mode::Advantage;
    request.spec.successThreshold = 3;
    job.StartRoll(request, csv, root);
    auto outcome = Wait(job);
    Check(outcome.saved && !outcome.cancelled && outcome.error.empty() && outcome.batch && outcome.batch->results.size() == 5 && outcome.entries.size() == 1,
        "Worker automatic export did not complete with retained history.");
    Check(outcome.entries[0].saved && outcome.entries[0].exportedPath == csv.wstring(), "Successful automatic export was not marked.");
    Check(Read(csv) == CsvBytes(outcome.entries), "Automatic export does not contain exactly the computed results.");
    CheckPersisted(root, outcome.entries[0]);
    auto sameBatch = outcome.entries[0];
    sameBatch.batch = *outcome.batch;
    SameResults(outcome.entries[0], sameBatch);

    const auto failureRoot = temporary.root / L"failure history";
    const auto invalidCsv = temporary.root / L"older.csv";
    Put(invalidCsv, "older header\nuntouched\n");
    job.StartRoll(MakeRequest(), invalidCsv, failureRoot);
    outcome = Wait(job);
    Check(!outcome.saved && !outcome.cancelled && !outcome.error.empty() && outcome.batch && outcome.batch->results.size() == 5 && outcome.entries.size() == 1,
        "Failed CSV export must retain all generated results.");
    Check(!outcome.entries[0].saved && Read(invalidCsv) == "older header\nuntouched\n", "Failed CSV export changed status or the old file.");
    const auto failed = outcome.entries[0];
    CheckPersisted(failureRoot, failed);
    const auto retryCsv = temporary.root / L"retry.csv";
    job.StartExport(outcome.entries, retryCsv, failureRoot);
    outcome = Wait(job);
    Check(outcome.saved && outcome.error.empty() && outcome.entries.size() == 1, "Durable retry failed.");
    SameResults(failed, outcome.entries[0]);
    CheckPersisted(failureRoot, outcome.entries[0]);
    Check(Read(retryCsv) == CsvBytes(outcome.entries), "Retry changed or duplicated dice results.");
    const auto retryBytes = Read(retryCsv);
    job.StartExport(outcome.entries, retryCsv, failureRoot);
    outcome = Wait(job);
    Check(outcome.saved && outcome.error.empty() && Read(retryCsv) == retryBytes, "Repeating a successful export duplicated a batch.");

    const auto cancelRoot = temporary.root / L"cancelled history";
    request = MakeRequest(L"1000d6kh500", 1000);
    job.StartRoll(request, std::nullopt, cancelRoot);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (job.Completed() < 2 && job.State() != Phase::Finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const auto observed = job.Completed();
    job.Cancel();
    outcome = Wait(job);
    Check(observed >= 2 && outcome.error.empty() && !outcome.saved && outcome.batch && outcome.batch->results.size() >= observed && outcome.entries.size() == 1,
        "Cancellation lost a completed roll or failed to checkpoint it.");
    // A cancellation that arrives after the last roll may legitimately complete.
    Check(outcome.cancelled || outcome.batch->results.size() == 1000, "Early cancellation was not reported.");
    CheckPersisted(cancelRoot, outcome.entries[0]);
    for (const auto& result : outcome.batch->results)
        Check(result.first.dice.size() == 1000 && std::count(result.first.kept.begin(), result.first.kept.end(), true) == 500,
            "Cancelled batch contains a partial dice group.");
    const auto cancelledEntry = outcome.entries[0];
    job.StartExport(outcome.entries, temporary.root / L"cancelled.csv", cancelRoot);
    outcome = Wait(job);
    Check(outcome.saved && outcome.error.empty(), "Cancelled partial batch could not be exported.");
    SameResults(cancelledEntry, outcome.entries[0]);

    const auto exportRoot = temporary.root / L"cancel export history";
    std::vector<HistoryEntry> entries{Fixture(300), Fixture(300)};
    for (const auto& entry : entries) SaveHistoryEntry(exportRoot, entry);
    const auto cancelCsv = temporary.root / L"cancel export.csv";
    job.StartExport(entries, cancelCsv, exportRoot);
    job.Cancel();
    outcome = Wait(job);
    Check(outcome.error.empty() && (outcome.cancelled || outcome.saved) && outcome.entries.size() == entries.size(), "Export cancellation returned an inconsistent outcome.");
    for (std::size_t i = 0; i < entries.size(); ++i) {
        SameResults(entries[i], outcome.entries[i]);
        CheckPersisted(exportRoot, outcome.entries[i]);
    }
    if (outcome.cancelled) {
        Check(!outcome.saved && !fs::exists(cancelCsv), "Cancelled export committed a partial CSV.");
        for (const auto& entry : outcome.entries) Check(!entry.saved, "Cancelled export marked an unexported batch saved.");
    } else Check(Read(cancelCsv) == CsvBytes(entries), "Late export cancellation left incomplete data.");

    // Simulate a process exit after CSV commit but before all local saved marks.
    // A read-only remaining checkpoint also verifies that retries preserve a
    // previously exported batch and report partial marking without duplicating.
    const auto markRoot = temporary.root / L"mark failure history";
    entries = {Fixture(2), Fixture(3)};
    for (const auto& entry : entries) SaveHistoryEntry(markRoot, entry);
    const auto markedCsv = temporary.root / L"marked.csv";
    ExportHistoryCsv(markedCsv, entries);
    const auto committedBytes = Read(markedCsv);
    entries[0].saved = true;
    entries[0].exportedPath = markedCsv.wstring();
    SaveHistoryEntry(markRoot, entries[0]);
    {
        ReadOnly readonly(markRoot / (Wide(entries[1].id) + L".dhist"));
        job.StartExport(entries, markedCsv, markRoot);
        outcome = Wait(job);
        Check(!outcome.saved && !outcome.error.empty() && outcome.entries[0].saved && !outcome.entries[1].saved,
            "A local marking/checkpoint failure must preserve per-batch export state.");
        Check(Read(markedCsv) == committedBytes, "A local history failure changed the committed CSV.");
    }
    job.StartExport(outcome.entries, markedCsv, markRoot);
    outcome = Wait(job);
    Check(outcome.saved && outcome.error.empty() && Read(markedCsv) == committedBytes, "Recovery after partial marking duplicated committed CSV rows.");
    for (const auto& entry : outcome.entries) { Check(entry.saved, "Recovered export not marked saved."); CheckPersisted(markRoot, entry); }

    for (const auto control : {wchar_t(0), wchar_t(9), wchar_t(31), wchar_t(127)}) {
        auto invalid = MakeRequest();
        invalid.name += control;
        bool rejected = false;
        try { job.StartRoll(invalid, std::nullopt, temporary.root / L"invalid metadata"); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected && job.State() == Phase::Idle && job.Completed() == 0, "Control metadata must be rejected before generating dice.");
        invalid = MakeRequest(); invalid.notation += control;
        rejected = false;
        try { job.StartRoll(invalid, std::nullopt, temporary.root / L"invalid metadata"); }
        catch (const std::exception&) { rejected = true; }
        Check(rejected && job.State() == Phase::Idle, "Control characters in notation must be rejected before generating dice.");
    }
    Check(!fs::exists(temporary.root / L"invalid metadata"), "Invalid metadata created a history checkpoint.");
}

#ifdef DICE_WORKER_HISTORY_STANDALONE
#include <iostream>
int main() {
    try { WorkerHistoryTests(); std::cout << "Worker history tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
#endif
