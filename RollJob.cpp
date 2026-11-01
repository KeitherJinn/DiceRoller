#include "RollJob.h"
#include <random>
#include <chrono>
#include <algorithm>
#include <set>

namespace dice {
namespace {
void CancelNow(const CancelCheck& cancelled) { if (cancelled && cancelled()) throw Cancelled(); }
void RememberId(std::vector<std::string>& ids, const std::string& id) {
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
}
}
RollJob::~RollJob() {
    Cancel();
    if (worker_.joinable()) worker_.join();
}
void RollJob::Prepare() {
    if (worker_.joinable()) throw std::logic_error("An operation is already running.");
    cancel_.store(false);
    completed_.store(0);
    outcome_ = {};
}
void RollJob::UpdateSummary(HistorySummary summary) {
    const auto found = std::find_if(outcome_.summaries.begin(), outcome_.summaries.end(),
        [&](const HistorySummary& current) { return current.id == summary.id; });
    if (found == outcome_.summaries.end()) outcome_.summaries.push_back(std::move(summary));
    else *found = std::move(summary);
}
void RollJob::AddError(const std::exception& error, const wchar_t* prefix) {
    if (!outcome_.error.empty()) outcome_.error += L"\n";
    outcome_.error += prefix;
    try { outcome_.error += Wide(error.what()); }
    catch (...) { outcome_.error += L"The operation failed; retained results remain available."; }
}
void RollJob::Save(const std::filesystem::path& path) {
    outcome_.path = path;
    phase_.store(Phase::Saving);
    AppendCsv(path, *outcome_.batch, [this] { return cancel_.load(); });
    outcome_.saved = true;
    outcome_.csvCommitted = true;
}
void RollJob::FinishError() {
    try { throw; }
    catch (const Cancelled&) { outcome_.cancelled = true; }
    catch (const std::exception& error) { AddError(error, L""); }
    catch (...) { outcome_.error += L"\nUnexpected operation failure."; }
}
bool RollJob::FlushDirty(const std::filesystem::path& historyRoot) {
    auto dirty = std::move(outcome_.dirtyEntries);
    outcome_.dirtyEntries.clear();
    const auto cancelled = [this] { return cancel_.load(); };
    for (auto& entry : dirty) {
        if (cancelled()) {
            outcome_.cancelled = true;
            auto summary = SummaryOf(entry); summary.localPersisted = false; UpdateSummary(std::move(summary));
            outcome_.dirtyEntries.push_back(std::move(entry));
            continue;
        }
        try {
            const auto summary = SaveHistoryEntry(historyRoot, entry, cancelled);
            UpdateSummary(summary); RememberId(outcome_.persistedIds, entry.id); completed_.fetch_add(1);
        } catch (const Cancelled&) {
            outcome_.cancelled = true;
            auto summary = SummaryOf(entry); summary.localPersisted = false; UpdateSummary(std::move(summary));
            outcome_.dirtyEntries.push_back(std::move(entry));
        } catch (const std::exception& error) {
            AddError(error, L"Local recovery could not be saved: ");
            auto summary = SummaryOf(entry); summary.localPersisted = false; UpdateSummary(std::move(summary));
            outcome_.dirtyEntries.push_back(std::move(entry));
        }
    }
    outcome_.localPersisted = outcome_.dirtyEntries.empty();
    return outcome_.localPersisted && !outcome_.cancelled;
}
bool RollJob::FlushMarks(const std::filesystem::path& historyRoot) {
    auto marks = std::move(outcome_.pendingMarks);
    outcome_.pendingMarks.clear();
    const auto cancelled = [this] { return cancel_.load(); };
    for (auto& mark : marks) {
        if (cancelled()) {
            outcome_.cancelled = true; outcome_.pendingMarks.push_back(std::move(mark)); continue;
        }
        try {
            auto entry = ReadHistoryEntry(historyRoot, mark.id, mark.archived, cancelled);
            entry.saved = true; entry.exportedPath = mark.exportedPath;
            const auto summary = SaveHistoryEntry(historyRoot, entry, cancelled);
            UpdateSummary(summary); RememberId(outcome_.persistedIds, mark.id); completed_.fetch_add(1);
        } catch (const Cancelled&) {
            outcome_.cancelled = true; outcome_.pendingMarks.push_back(std::move(mark));
        } catch (const std::exception& error) {
            AddError(error, L"CSV export status could not be saved: ");
            outcome_.pendingMarks.push_back(std::move(mark));
        }
    }
    return outcome_.pendingMarks.empty() && !outcome_.cancelled;
}
void RollJob::StartRoll(Request request, std::optional<std::filesystem::path> path,
                        std::filesystem::path historyRoot) {
    Validate(request);
    Prepare();
    auto batch = std::make_shared<Batch>();
    batch->request = std::move(request);
    outcome_.batch = batch;
    HistoryEntry record;
    if (!historyRoot.empty()) record = NewHistoryEntry(*batch);
    phase_.store(Phase::Rolling);
    try {
        worker_ = std::thread([this, batch, path, historyRoot, record = std::move(record)]() mutable {
            auto checkpoint = [&] {
                if (historyRoot.empty() || batch->results.empty()) return;
                record.batch = *batch;
                // A single bounded final checkpoint must survive cancellation of
                // rolling; closing no longer rewrites already-persisted history.
                const auto summary = SaveHistoryEntry(historyRoot, record);
                record.saved = summary.saved; record.exportedPath = summary.exportedPath;
                UpdateSummary(summary); RememberId(outcome_.persistedIds, record.id);
                outcome_.localPersisted = true;
            };
            try {
                std::random_device seed;
                std::mt19937 engine(seed());
                const NextDie next = [&engine](int sides) {
                    return std::uniform_int_distribution<int>(1, sides)(engine);
                };
                batch->results.reserve(batch->request.times);
                auto lastCheckpoint = std::chrono::steady_clock::now();
                for (int i = 0; i < batch->request.times; ++i) {
                    auto result = Roll(batch->request.spec, batch->request.mode, next,
                                       [this] { return cancel_.load(); });
                    result.timestamp = Timestamp();
                    batch->results.push_back(std::move(result));
                    completed_.store(batch->results.size());
                    if (!historyRoot.empty()) outcome_.localPersisted = false;
                    if (!historyRoot.empty() && std::chrono::steady_clock::now() - lastCheckpoint > std::chrono::milliseconds(500)) {
                        checkpoint(); lastCheckpoint = std::chrono::steady_clock::now();
                    }
                }
                checkpoint();
                if (cancel_.load()) throw Cancelled();
                if (path) {
                    if (historyRoot.empty()) Save(*path);
                    else ExportIds(*path, historyRoot);
                }
            } catch (...) {
                FinishError();
                if (!historyRoot.empty() && !batch->results.empty() && !outcome_.localPersisted) {
                    try { checkpoint(); }
                    catch (const std::exception& error) {
                        AddError(error, L"Local recovery failed: ");
                        record.batch = *batch;
                        auto summary = SummaryOf(record); summary.localPersisted = false; UpdateSummary(std::move(summary));
                        outcome_.dirtyEntries.push_back(std::move(record));
                        outcome_.localPersisted = false;
                    }
                }
            }
            phase_.store(Phase::Finished);
        });
    } catch (...) { phase_.store(Phase::Idle); outcome_ = {}; throw; }
}
void RollJob::ExportIds(const std::filesystem::path& path, const std::filesystem::path& historyRoot) {
    outcome_.path = path;
    phase_.store(Phase::Saving);
    if (!FlushDirty(historyRoot)) return;
    const auto cancelled = [this] { return cancel_.load(); };
    CancelNow(cancelled);
    // Existing durable records are read on demand, never rewritten as an export
    // preflight and never accumulated as a vector of complete dice batches.
    ExportHistoryCsvIds(path, historyRoot, outcome_.summaries, cancelled);
    outcome_.csvCommitted = true;
    for (const auto& summary : outcome_.summaries) {
        auto mark = summary; mark.saved = true; mark.exportedPath = path.wstring();
        outcome_.pendingMarks.push_back(std::move(mark));
    }
    outcome_.saved = FlushMarks(historyRoot);
}
void RollJob::Export(const std::filesystem::path& path, const std::filesystem::path& historyRoot) {
    ExportIds(path, historyRoot);
    // Compatibility for callers of the older full-entry API. Production ID-based
    // export returns only summaries, dirty result data and pending metadata marks.
    for (auto& entry : outcome_.entries) {
        const auto found = std::find_if(outcome_.summaries.begin(), outcome_.summaries.end(),
            [&](const HistorySummary& summary) { return summary.id == entry.id; });
        if (found != outcome_.summaries.end()) {
            entry.saved = found->saved; entry.exportedPath = found->exportedPath; entry.archived = found->archived;
        }
    }
}
void RollJob::StartExport(std::vector<HistoryEntry> entries, std::filesystem::path path,
                          std::filesystem::path historyRoot) {
    if (entries.empty()) throw std::invalid_argument("Select one or more batches to export.");
    Prepare();
    for (const auto& entry : entries) UpdateSummary(SummaryOf(entry));
    outcome_.entries = entries;
    outcome_.dirtyEntries = std::move(entries);
    outcome_.localPersisted = false;
    phase_.store(Phase::Saving);
    try {
        worker_ = std::thread([this, path, historyRoot] {
            try { Export(path, historyRoot); } catch (...) { FinishError(); }
            phase_.store(Phase::Finished);
        });
    } catch (...) { phase_.store(Phase::Idle); outcome_ = {}; throw; }
}
void RollJob::StartExportIds(std::vector<HistorySummary> entries, std::filesystem::path path,
                             std::filesystem::path historyRoot, std::vector<HistoryEntry> dirty) {
    if (entries.empty()) throw std::invalid_argument("Select one or more batches to export.");
    std::set<std::string> selected;
    for (const auto& entry : entries) if (!selected.insert(entry.id).second)
        throw std::invalid_argument("The same batch was selected more than once.");
    for (const auto& entry : dirty) if (!selected.count(entry.id))
        throw std::invalid_argument("Only selected dirty batches may be flushed for this export.");
    Prepare();
    outcome_.summaries = std::move(entries);
    outcome_.dirtyEntries = std::move(dirty);
    outcome_.localPersisted = outcome_.dirtyEntries.empty();
    phase_.store(Phase::Saving);
    try {
        worker_ = std::thread([this, path, historyRoot] {
            try { ExportIds(path, historyRoot); } catch (...) { FinishError(); }
            phase_.store(Phase::Finished);
        });
    } catch (...) { phase_.store(Phase::Idle); outcome_ = {}; throw; }
}
void RollJob::StartFlushDirty(std::vector<HistoryEntry> dirty, std::filesystem::path historyRoot,
                              std::vector<HistorySummary> pendingMarks) {
    Prepare();
    for (const auto& entry : dirty) {
        auto summary = SummaryOf(entry); summary.localPersisted = false; UpdateSummary(std::move(summary));
    }
    for (const auto& mark : pendingMarks) UpdateSummary(mark);
    outcome_.dirtyEntries = std::move(dirty);
    outcome_.pendingMarks = std::move(pendingMarks);
    outcome_.localPersisted = outcome_.dirtyEntries.empty();
    phase_.store(Phase::Saving);
    try {
        worker_ = std::thread([this, historyRoot] {
            try {
                const bool records = FlushDirty(historyRoot);
                const bool marks = FlushMarks(historyRoot);
                outcome_.saved = records && marks;
            } catch (...) { FinishError(); }
            phase_.store(Phase::Finished);
        });
    } catch (...) { phase_.store(Phase::Idle); outcome_ = {}; throw; }
}
void RollJob::StartSave(std::shared_ptr<const Batch> batch, std::filesystem::path path) {
    if (!batch || batch->results.empty()) throw std::invalid_argument("There are no results to save.");
    Prepare();
    outcome_.batch = std::move(batch);
    phase_.store(Phase::Saving);
    try {
        worker_ = std::thread([this, path] {
            try { Save(path); } catch (...) { FinishError(); }
            phase_.store(Phase::Finished);
        });
    } catch (...) { phase_.store(Phase::Idle); outcome_ = {}; throw; }
}
Outcome RollJob::Take() {
    if (State() != Phase::Finished) throw std::logic_error("The operation is not finished.");
    worker_.join();
    auto result = std::move(outcome_);
    phase_.store(Phase::Idle);
    return result;
}
}
