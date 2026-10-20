#include "RollJob.h"
#include <random>

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
void RollJob::StartRoll(Request request, std::optional<std::filesystem::path> path) {
    Validate(request);
    Prepare();
    auto batch = std::make_shared<Batch>();
    batch->request = std::move(request);
    outcome_.batch = batch;
    phase_.store(Phase::Rolling);
    try {
        worker_ = std::thread([this, batch, path] {
            try {
                std::random_device seed;
                std::mt19937 engine(seed());
                const NextDie next = [&engine](int sides) {
                    return std::uniform_int_distribution<int>(1, sides)(engine);
                };
                batch->results.reserve(batch->request.times);
                for (int i = 0; i < batch->request.times; ++i) {
                    auto result = Roll(batch->request.spec, batch->request.mode, next,
                                       [this] { return cancel_.load(); });
                    result.timestamp = Timestamp();
                    batch->results.push_back(std::move(result));
                    completed_.store(batch->results.size());
                }
                if (cancel_.load()) throw Cancelled();
                if (path) Save(*path);
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
