#include "DiceCore.h"
#include "ResultStore.h"
#include "RollJob.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

namespace fs = std::filesystem;
using namespace dice;
void UiRegressionTests(const std::filesystem::path& base);
void SettingsRegressionTests(const std::filesystem::path& base);
void UiFeatureTests(const std::filesystem::path& base);
void UiCloseTests(const std::filesystem::path& base);
void UiViewportTests(const std::filesystem::path& base);
void AnalysisTests();
void HistoryTests();
void PresetTests();
void FeatureWindowTests();
void WorkerHistoryTests();
void UiAdvancedTests(const std::filesystem::path& base);
namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void Throws(F action) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    Require(threw, "Expected an exception.");
}
std::string Read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("Test file could not be read.");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void Put(const fs::path& file, const std::string& content) {
    std::ofstream out(file, std::ios::binary);
    out << content;
    out.close();
    Require(!out.fail(), "Test fixture could not be written.");
}
struct TempDirectory {
    fs::path parent;
    fs::path root;
    TempDirectory() {
        parent = fs::temp_directory_path();
        GUID id{};
        Require(SUCCEEDED(CoCreateGuid(&id)), "GUID creation failed.");
        wchar_t text[40]{};
        StringFromGUID2(id, text, 40);
        root = parent / (std::wstring(L"DiceRollerTests-") + text);
        Require(fs::create_directory(root), "Temporary test directory already exists.");
    }
    ~TempDirectory() {
        // Only delete the exact, uniquely created child of the OS temporary directory.
        if (root.parent_path() == parent && root.filename().wstring().find(L"DiceRollerTests-{") == 0) {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
    }
};
Batch Fixture(Mode mode = Mode::Normal) {
    Batch batch;
    batch.request = {L"骰子 \"Alice\",😀", L"2d6+3", {2, 6, 3}, mode, 1, false};
    std::vector<int> values{1, 6, 2, 3};
    std::size_t index = 0;
    auto result = Roll(batch.request.spec, mode, [&](int) { return values.at(index++); });
    result.timestamp = "2026-10-20 12:34:56";
    batch.results.push_back(std::move(result));
    return batch;
}
Outcome Wait(RollJob& job) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (job.State() != Phase::Finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(job.State() == Phase::Finished, "Worker did not finish within ten seconds.");
    return job.Take();
}
}

int main() {
    int passed = 0;
    int failed = 0;
    auto test = [&](const char* name, auto action) {
        try { action(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    };
    test("notation variants", [] {
        for (const auto* text : {L"2d6+3", L"d20-1", L"3D8", L"d12", L" 2d6 + 3 "}) {
            const auto spec = Parse(text);
            Require(spec.count > 0 && spec.sides > 0, "Valid notation rejected.");
        }
        Require(Parse(L"d20-1").modifier == -1 && Parse(L"3D8").count == 3, "Incorrect parse.");
    });
    test("invalid and huge notation", [] {
        for (const auto* text : {L"", L"2", L"d", L"0d6", L"2d0", L"-2d6", L"2d-6", L"2d6++1",
            L"2dd6", L"2d6xyz", L"2d6+", L"1001d6", L"1d2147483648", L"99999999999999999999d6"})
            Throws([&] { Parse(text); });
    });
    test("times validation and whitespace", [] {
        Require(ParseTimes(L" 10000 ") == 10000, "Boundary failed.");
        for (const auto* text : {L"0", L"-1", L"10001", L"1.5", L"2x", L""}) Throws([&] { ParseTimes(text); });
    });
    test("original int32 overflow regression", [] {
        auto result = Roll(Parse(L"1d1+2147483647"), Mode::Normal, [](int) { return 1; });
        Require(result.finalValue == 2147483648LL, "Result overflowed int32.");
        result = Roll(Parse(L"2d2147483647"), Mode::Normal, [](int sides) { return sides; });
        Require(result.finalValue == 4294967294LL, "Dice sum overflowed int32.");
    });
    test("int64 boundaries and checked addition", [] {
        Require(Roll(Parse(L"1d1+9223372036854775806"), Mode::Normal, [](int) { return 1; }).finalValue == INT64_MAX, "Max result failed.");
        Require(Roll(Parse(L"1d1-9223372036854775808"), Mode::Normal, [](int) { return 1; }).finalValue == INT64_MIN + 1, "Min modifier failed.");
        Throws([] { Parse(L"1d1+9223372036854775807"); });
        Throws([] { Parse(L"1d1-9223372036854775809"); });
        Throws([] { CheckedAdd(INT64_MAX, 1); });
        Throws([] { CheckedAdd(INT64_MIN, -1); });
    });
    test("total workload includes both groups", [] {
        Validate({1000, 6, 0}, 1000, Mode::Normal);
        Validate({1000, 6, 0}, 500, Mode::Advantage);
        Throws([] { Validate({1000, 6, 0}, 501, Mode::Disadvantage); });
        Throws([] { Validate({1000, 6, 0}, 1001, Mode::Normal); });
    });
    test("request name validation", [] {
        auto request = Fixture().request;
        Validate(request);
        request.name = L" \t "; Throws([&] { Validate(request); });
        request.name = std::wstring(129, L'a'); Throws([&] { Validate(request); });
        request.name = L"a\nb"; Throws([&] { Validate(request); });
    });
    test("normal roll uses only one group", [] {
        const auto batch = Fixture();
        const auto& result = batch.results.front();
        Require(result.first.total == 7 && !result.second && result.finalValue == 10, "Incorrect normal roll.");
    });
    test("advantage preserves ordered rolls", [] {
        const auto result = Fixture(Mode::Advantage).results.front();
        Require(result.first.total == 7 && result.second->total == 5 && result.chosen == 7 && result.finalValue == 10, "Incorrect advantage.");
    });
    test("disadvantage preserves ordered rolls", [] {
        const auto result = Fixture(Mode::Disadvantage).results.front();
        Require(result.first.total == 7 && result.second->total == 5 && result.chosen == 5 && result.finalValue == 8, "Incorrect disadvantage.");
    });
    test("equal groups and negative modifier", [] {
        const auto result = Roll({2, 6, -10}, Mode::Advantage, [](int) { return 3; });
        Require(result.chosen == 6 && result.finalValue == -4, "Tie or modifier failed.");
    });
    test("bad random source rejected", [] {
        Throws([] { Roll({1, 6, 0}, Mode::Normal, [](int) { return 7; }); });
        Throws([] { Roll({1, 6, 0}, Mode::Normal, [](int) { return 0; }); });
    });
    test("cancellation within a group", [] {
        int calls = 0;
        bool cancelled = false;
        try { Roll({1000, 6, 0}, Mode::Normal, [&](int) { ++calls; return 1; }, [&] { return calls == 3; }); }
        catch (const Cancelled&) { cancelled = true; }
        Require(cancelled && calls == 3, "Cancellation did not stop dice generation.");
    });
    test("UTF-8 Chinese and surrogate pairs", [] {
        Require(Utf8(L"骰子") == "\xE9\xAA\xB0\xE5\xAD\x90", "Incorrect Chinese UTF-8.");
        const std::wstring text = L"骰子😀";
        Require(Wide(Utf8(text)) == text, "Unicode round trip failed.");
        Throws([] { Utf8(std::wstring(1, wchar_t(0xD800))); });
        Throws([] { Wide("\xFF"); });
    });
    test("CSV double quotes commas and newlines", [] {
        Require(CsvField("A\",B") == "\"A\"\",B\"", "Quotes not doubled.");
        Require(CsvField("a\r\nb") == "\"a\r\nb\"", "Newline not enclosed.");
        const auto batch = Fixture(Mode::Advantage);
        const auto row = CsvRow(batch.request, batch.results.front());
        Require(row.find(",2,6,7,5,7,3,10,") != std::string::npos, "CSV values/order incorrect.");
        Require(row.find("\"1,6\",\"2,3\"\r\n") != std::string::npos, "Dice details missing.");
    });
    test("zero modifier formatting and timestamp option", [] {
        Request request{L"roll", L"1d1+0", Parse(L"1d1+0"), Mode::Normal, 1, false};
        auto result = Roll(request.spec, request.mode, [](int) { return 1; });
        result.timestamp = "2026-10-20 12:34:56";
        Require(FormatResult(request, result, 1) == L"Roll 1 for roll [1d1+0] (1) = 1 -> Final: 1\r\n", "Extra parenthesis regression.");
        request.showTimestamp = true;
        Require(FormatResult(request, result, 1).find(L" @ 2026-10-20") != std::wstring::npos, "Timestamp missing.");
    });
    test("display detail bounded but CSV complete", [] {
        Request request{L"roll", L"1000d1", {1000, 1, 0}, Mode::Normal, 1, false};
        const auto result = Roll(request.spec, request.mode, [](int) { return 1; });
        Require(FormatResult(request, result, 1).find(L"970 more") != std::wstring::npos, "Display not shortened.");
        Require(CsvRow(request, result).find("more") == std::string::npos, "CSV details shortened.");
    });
    test("extension checks filename only", [] {
        const fs::path base = L"C:\\base";
        Require(ResolvePath(L"C:\\logs.v1\\rolls", base) == fs::path(L"C:\\logs.v1\\rolls.csv"), "Directory dot regression.");
        Require(ResolvePath(L"results.CSV", base).filename() == L"results.CSV", "Existing extension changed.");
        Require(ResolvePath(L"sub\\rolls", base) == fs::path(L"C:\\base\\sub\\rolls.csv"), "Relative path not anchored.");
        for (const auto* text : {L"", L"..", L"C:rolls", L"\\rolls", L"test.exe", L"folder\\"})
            Throws([&] { ResolvePath(text, base); });
    });
    test("default path independent of cwd", [] {
        const auto first = DefaultDirectory();
        const auto old = fs::current_path();
        TempDirectory temp;
        fs::current_path(temp.root);
        fs::path second;
        try { second = DefaultDirectory(); } catch (...) { fs::current_path(old); throw; }
        fs::current_path(old);
        Require(first.is_absolute() && first == second, "Default path depends on cwd.");
    });
    test("Unicode path creation and UTF-8 BOM", [] {
        TempDirectory temp;
        const auto path = temp.root / L"中文目录" / L"骰子😀.csv";
        const auto batch = Fixture();
        AppendCsv(path, batch);
        Require(Read(path) == std::string("\xEF\xBB\xBF") + CsvHeader() + "\r\n" + CsvRow(batch.request, batch.results.front()), "Stored bytes differ.");
    });
    test("append preserves records and single header", [] {
        TempDirectory temp;
        const auto path = temp.root / L"append.csv";
        const auto batch = Fixture();
        AppendCsv(path, batch);
        const auto before = Read(path);
        AppendCsv(path, batch);
        Require(Read(path) == before + CsvRow(batch.request, batch.results.front()), "Append replaced or duplicated header.");
    });
    test("missing last newline repaired", [] {
        TempDirectory temp;
        const auto path = temp.root / L"newline.csv";
        Put(path, CsvHeader());
        const auto batch = Fixture();
        AppendCsv(path, batch);
        Require(Read(path) == CsvHeader() + "\r\n" + CsvRow(batch.request, batch.results.front()), "Record fused to header.");
    });
    test("legacy header rejected without mutation", [] {
        TempDirectory temp;
        const auto path = temp.root / L"legacy.csv";
        const std::string old = "Name,DiceNotation,Mode,NumDice,Sides,BaseRoll,AltRoll,Modifier,FinalResult,Timestamp\nlegacy\n";
        Put(path, old);
        Throws([&] { AppendCsv(path, Fixture()); });
        Require(Read(path) == old, "Legacy CSV modified.");
    });
    test("legacy export cannot silently drop advanced rule metadata", [] {
        TempDirectory temp;
        auto batch = Fixture();
        batch.request.target = {true, Compare::AtLeast, 5};
        Throws([&] { AppendCsv(temp.root / L"legacy.csv", batch); });
        Require(!fs::exists(temp.root / L"legacy.csv"), "Advanced rules were exported in an incomplete legacy format.");
    });
    test("cancellation before commit preserves original", [] {
        TempDirectory temp;
        const auto path = temp.root / L"cancel.csv";
        AppendCsv(path, Fixture());
        const auto old = Read(path);
        int checks = 0;
        Throws([&] { AppendCsv(path, Fixture(), [&] { return ++checks == 5; }); });
        Require(Read(path) == old, "Cancelled append changed original.");
        Require(std::distance(fs::directory_iterator(temp.root), fs::directory_iterator()) == 1, "Staging or lock file leaked.");
    });
    test("commit failure preserves file and permits retry", [] {
        TempDirectory temp;
        const auto path = temp.root / L"readonly.csv";
        const auto batch = Fixture();
        AppendCsv(path, batch);
        const auto old = Read(path);
        Require(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY), "Could not mark fixture read-only.");
        bool threw = false;
        try { AppendCsv(path, batch); } catch (const std::exception&) { threw = true; }
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        Require(threw && Read(path) == old, "Commit failure changed original or went unnoticed.");
        AppendCsv(path, batch);
        Require(Read(path) == old + CsvRow(batch.request, batch.results.front()), "Retry duplicated or lost data.");
    });
    test("locked file reports failure without mutation", [] {
        TempDirectory temp;
        const auto path = temp.root / L"locked.csv";
        AppendCsv(path, Fixture());
        const auto before = Read(path);
        const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(handle != INVALID_HANDLE_VALUE, "Fixture lock failed.");
        bool threw = false;
        try { AppendCsv(path, Fixture()); } catch (const std::exception&) { threw = true; }
        CloseHandle(handle);
        Require(threw && Read(path) == before, "Locked file was changed or failure hidden.");
    });
    test("pre-existing companion file is never deleted", [] {
        TempDirectory temp;
        const auto path = temp.root / L"data.csv";
        const auto companion = temp.root / L"data.csv.lock";
        Put(companion, "pre-existing user content");
        Throws([&] { AppendCsv(path, Fixture()); });
        Require(Read(companion) == "pre-existing user content" && !fs::exists(path), "Existing companion was touched.");
    });
    test("invalid parent and empty batch rejected", [] {
        TempDirectory temp;
        Put(temp.root / L"file", "not a directory");
        Throws([&] { AppendCsv(temp.root / L"file" / L"a.csv", Fixture()); });
        Throws([&] { AppendCsv(temp.root / L"empty.csv", Batch{}); });
        Require(!fs::exists(temp.root / L"empty.csv"), "Empty batch created a file.");
    });
    test("log retention and clear", [] {
        LogBuffer log;
        for (int i = 0; i < 600; ++i) log.Add(std::to_wstring(i) + L"\r\n");
        Require(log.Dropped() == 100 && log.Text().find(L"100\r\n") == 0, "Entry retention failed.");
        log.Add(std::wstring(200000, L'a'));
        Require(log.Text().size() <= LogBuffer::MaxCharacters, "Character budget exceeded.");
        log.Clear(); Require(log.Text().empty() && log.Dropped() == 0, "Clear failed.");
    });
    test("worker completion and explicit save", [] {
        TempDirectory temp;
        RollJob job;
        Request request{L"test", L"1d1+2147483647", Parse(L"1d1+2147483647"), Mode::Normal, 3, false};
        job.StartRoll(request, std::nullopt);
        auto outcome = Wait(job);
        Require(!outcome.saved && outcome.error.empty() && outcome.batch->results.size() == 3, "Worker roll failed.");
        Require(outcome.batch->results[0].finalValue == 2147483648LL, "Worker result incorrect.");
        job.StartSave(outcome.batch, temp.root / L"worker.csv");
        outcome = Wait(job);
        Require(outcome.saved && fs::exists(temp.root / L"worker.csv"), "Explicit save failed.");
    });
    test("worker save failure retains exact results for retry", [] {
        TempDirectory temp;
        const auto path = temp.root / L"old.csv";
        Put(path, "wrong header\n");
        RollJob job;
        auto request = Fixture().request;
        job.StartRoll(request, path);
        auto outcome = Wait(job);
        Require(!outcome.saved && !outcome.error.empty() && outcome.batch->results.size() == 1, "Failed batch not retained.");
        const auto retained = outcome.batch;
        job.StartSave(retained, temp.root / L"retry.csv");
        outcome = Wait(job);
        Require(outcome.saved && outcome.batch == retained, "Retry re-rolled or changed batch.");
        Require(Read(path) == "wrong header\n", "Legacy file changed.");
    });
    test("native UI layout, DPI, keyboard, background roll and retained-batch save", [] {
        TempDirectory temp;
        UiRegressionTests(temp.root);
    });
    test("settings Unicode, validation, corruption fallback and atomic save", [] {
        TempDirectory temp;
        SettingsRegressionTests(temp.root);
    });
    test("quick dice, presets, settings memory and browse recovery", [] {
        TempDirectory temp;
        UiFeatureTests(temp.root);
    });
    test("closing while confirmation pumps completed work", [] {
        TempDirectory temp;
        UiCloseTests(temp.root);
    });
    test("high DPI work area, scroll reachability and window placement", [] {
        TempDirectory temp;
        UiViewportTests(temp.root);
    });
    test("mixed dice, keep rules, pools, targets, statistics and probability", [] { AnalysisTests(); });
    test("durable history, recovery checkpoints and idempotent combined export", [] { HistoryTests(); });
    test("preset exchange, groups, duplicate strategies and atomic preservation", [] { PresetTests(); });
    test("history and probability native windows, DPI, charts and cancellation", [] { FeatureWindowTests(); });
    test("worker checkpoints, failed export recovery and exact-result retries", [] { WorkerHistoryTests(); });
    test("continuous batches, target rules, recovery and combined export UI integration", [] {
        TempDirectory temp; UiAdvancedTests(temp.root);
    });
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
