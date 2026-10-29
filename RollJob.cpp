#include "RollJob.h"
#include <random>
#include <chrono>

namespace dice {
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
void RollJob::Save(const std::filesystem::path& path) {
    outcome_.path = path;
    phase_.store(Phase::Saving);
    AppendCsv(path, *outcome_.batch, [this] { return cancel_.load(); });
    outcome_.saved = true;
}
void RollJob::FinishError() {
    try { throw; }
    catch (const Cancelled&) { outcome_.cancelled = true; }
    catch (const std::exception& error) {
        try { outcome_.error = Wide(error.what()); }
        catch (...) { outcome_.error = L"The operation failed. Results already computed remain available."; }
    }
    catch (...) { outcome_.error = L"Unexpected operation failure."; }
}
void RollJob::StartRoll(Request request, std::optional<std::filesystem::path> path,
                        std::filesystem::path historyRoot) {
    Validate(request);
    Prepare();
    auto batch = std::make_shared<Batch>();
    batch->request = std::move(request);
    outcome_.batch = batch;
    // Allocate and validate the recovery identity before generating any randomness.
    if (!historyRoot.empty()) outcome_.entries.push_back(NewHistoryEntry(*batch));
    phase_.store(Phase::Rolling);
    try {
        worker_ = std::thread([this, batch, path, historyRoot] {
            auto checkpoint = [&] {
                if (historyRoot.empty() || batch->results.empty()) return;
                outcome_.entries.front().batch = *batch;
                SaveHistoryEntry(historyRoot, outcome_.entries.front());
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
                    if (!historyRoot.empty() && std::chrono::steady_clock::now() - lastCheckpoint > std::chrono::milliseconds(500)) {
                        checkpoint();
                        lastCheckpoint = std::chrono::steady_clock::now();
                    }
                }
                checkpoint();
                if (cancel_.load()) throw Cancelled();
                if (path) {
                    if (historyRoot.empty()) Save(*path);
                    else Export(*path, historyRoot);
                }
            } catch (...) {
                FinishError();
                // Cancellation still checkpoints completed rolls; never invent results.
                try { checkpoint(); }
                catch (const std::exception& e) { outcome_.error += L"\nLocal recovery failed: " + Wide(e.what()); }
            }
            phase_.store(Phase::Finished);
        });
    } catch (...) { phase_.store(Phase::Idle); outcome_ = {}; throw; }
}
void RollJob::Export(const std::filesystem::path& path, const std::filesystem::path& historyRoot) {
    outcome_.path = path;
    phase_.store(Phase::Saving);
    // Persist every selected batch before touching the export destination.
    for (const auto& entry : outcome_.entries) SaveHistoryEntry(historyRoot, entry);
    ExportHistoryCsv(path, outcome_.entries, [this] { return cancel_.load(); });
    for (auto& entry : outcome_.entries) {
        auto marked = entry;
        marked.saved = true;
        marked.exportedPath = path.wstring();
        SaveHistoryEntry(historyRoot, marked);
        entry = std::move(marked);
    }
    outcome_.saved = true;
}
void RollJob::StartExport(std::vector<HistoryEntry> entries, std::filesystem::path path,
                          std::filesystem::path historyRoot) {
    if (entries.empty()) throw std::invalid_argument("Select one or more batches to export.");
    Prepare();
    outcome_.entries = std::move(entries);
    phase_.store(Phase::Saving);
    try {
        worker_ = std::thread([this, path, historyRoot] {
            try { Export(path, historyRoot); } catch (...) { FinishError(); }
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
