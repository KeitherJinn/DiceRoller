#pragma once
#include "ResultStore.h"
#include "HistoryStore.h"
#include <atomic>
#include <memory>
#include <thread>

namespace dice {
enum class Phase { Idle, Rolling, Saving, Finished };
struct Outcome {
    std::shared_ptr<const Batch> batch;
    std::filesystem::path path;
    bool saved = false;
    bool cancelled = false;
    std::wstring error;
    std::vector<HistoryEntry> entries;
};
// No HWND crosses threads. Results are read only after Finished and join().
class RollJob {
public:
    ~RollJob();
    void StartRoll(Request request, std::optional<std::filesystem::path> path,
                   std::filesystem::path historyRoot = {});
    void StartSave(std::shared_ptr<const Batch> batch, std::filesystem::path path);
    void StartExport(std::vector<HistoryEntry> entries, std::filesystem::path path,
                     std::filesystem::path historyRoot);
    void Cancel() { cancel_.store(true); }
    Phase State() const { return phase_.load(); }
    std::size_t Completed() const { return completed_.load(); }
    Outcome Take();
private:
    void Prepare();
    void Save(const std::filesystem::path& path);
    void Export(const std::filesystem::path& path, const std::filesystem::path& historyRoot);
    void FinishError();
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<std::size_t> completed_{0};
    std::atomic<Phase> phase_{Phase::Idle};
    Outcome outcome_;
};
}
