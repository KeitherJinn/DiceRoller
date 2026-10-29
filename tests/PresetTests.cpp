#include "PresetExchange.h"
#include "ResultStore.h"
#include <windows.h>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
namespace fs = std::filesystem;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void Throws(Function action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "Invalid preset operation was accepted.");
}
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "Could not open preset test fixture.");
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
void Put(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    Require(!output.fail(), "Could not write preset test fixture.");
}
bool Same(const dice::Preset& a, const dice::Preset& b) {
    return a.name == b.name && a.group == b.group && a.notation == b.notation && a.times == b.times &&
        a.mode == b.mode && a.successThreshold == b.successThreshold && a.target.enabled == b.target.enabled &&
        a.target.comparison == b.target.comparison && a.target.value == b.target.value;
}
struct Directory {
    fs::path parent = fs::temp_directory_path();
    fs::path path;
    Directory() {
        GUID id{};
        Require(SUCCEEDED(CoCreateGuid(&id)), "Could not create preset test directory identity.");
        wchar_t guid[40]{};
        Require(StringFromGUID2(id, guid, 40) != 0, "Could not format preset test directory identity.");
        path = parent / (std::wstring(L"DiceRollerPresetTests-") + guid);
        Require(fs::create_directory(path), "Preset test directory already exists.");
    }
    ~Directory() {
        if (path.parent_path() == parent && path.filename().wstring().find(L"DiceRollerPresetTests-{") == 0) {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    }
};
}

void PresetTests() {
    using namespace dice;
    Directory directory;
    const auto path = directory.path / L"角色预设 😀.drpresets";
    const auto badPath = directory.path / L"invalid.drpresets";
    const std::vector<Preset> original{
        { L"攻击 😀", L"d20+5", 2, Mode::Advantage, L"角色 A", 0, { true, Compare::AtLeast, 15 } },
        { L"骰池", L"8d10", 4, Mode::Normal, L"角色 B", 8, { true, Compare::Equal, 3 } },
        { L"混合", L"2d6+1d4+3", 1, Mode::Normal, L"伤害", 0, { true, Compare::Less, -5 } },
        { L"保留", L"4d6kh3", 1, Mode::Normal, L"", 0, { false, Compare::Greater, (std::numeric_limits<std::int64_t>::min)() } }
    };
    ExportPresets(path, original);
    const auto restored = ReadPresets(path);
    Require(restored.size() == original.size(), "Preset export lost records.");
    for (std::size_t i = 0; i < original.size(); ++i)
        Require(Same(restored[i], original[i]), "Preset names, groups or rule settings changed during export/import.");
    const auto bytes = Read(path);
    Require(bytes.find(Utf8(original.front().name)) != std::string::npos, "Preset files must contain valid UTF-8 names.");
    Put(badPath, std::string("\xEF\xBB\xBF") + bytes);
    Require(ReadPresets(badPath).size() == original.size(), "UTF-8 BOM imports must be accepted.");
    auto lf = bytes;
    for (auto pos = lf.find('\r'); pos != std::string::npos; pos = lf.find('\r')) lf.erase(pos, 1);
    Put(badPath, lf);
    Require(ReadPresets(badPath).size() == original.size(), "LF line endings must be accepted.");

    auto reject = [&](const std::string& input) {
        Put(badPath, input);
        Throws([&] { ReadPresets(badPath); });
        Require(Read(badPath) == input && Read(path) == bytes, "Import validation must never mutate source or current files.");
    };
    for (const auto& input : { std::string(), std::string("DiceRollerPresets\t99\n"),
        std::string("DiceRollerPresets\t1\nname\tgroup\td6\t1\t0\t0\t0\t0"),
        std::string("DiceRollerPresets\t1\nname\tgroup\td6\t1\t0\t0\t0\t0\t0\textra\n"),
        std::string("DiceRollerPresets\t1\nname\tgroup\td6\t0\t0\t0\t0\t0\t0\n"),
        std::string("DiceRollerPresets\t1\nname\tgroup\td6\t1\t99\t0\t0\t0\t0\n"),
        std::string("DiceRollerPresets\t1\nname\tgroup\td6\t1\t0\t-1\t0\t0\t0\n"),
        std::string("DiceRollerPresets\t1\nname\tgroup\td6\t1\t0\t0\t2\t0\t0\n"),
        std::string("DiceRollerPresets\t1\nname\tgroup\td6\t1\t0\t0\t0\t99\t0\n"),
        std::string("DiceRollerPresets\t1\nname\tgroup\td6\t1\t0\t0\t0\t0\t9223372036854775808\n"),
        std::string("DiceRollerPresets\t1\nname\tgroup\td6\t1x\t0\t0\t0\t0\t0\n"),
        std::string("DiceRollerPresets\t1\nname\tgroup\td6+1\t1\t0\t3\t0\t0\t0\n"),
        std::string(256 * 1024 + 1, 'x') }) reject(input);
    auto invalidUtf8 = bytes;
    invalidUtf8[invalidUtf8.find('\n') + 1] = static_cast<char>(0xff);
    reject(invalidUtf8);
    reject(bytes + "invalid late record\n");
    Throws([&] { ReadPresets(directory.path / L"missing.drpresets"); });

    const std::vector<Preset> current{ { L"Attack", L"d20+1", 1, Mode::Normal, L"old" },
        { L"Attack (2)", L"d4", 1, Mode::Normal } };
    const std::vector<Preset> incoming{ { L" attack ", L"d6", 1, Mode::Normal, L" new " },
        { L"Heal", L"d8", 1, Mode::Normal }, { L"HEAL", L"d10", 1, Mode::Normal } };
    const auto preview = PreviewImport(current, incoming);
    Require(preview.existingCount == 2 && preview.incomingCount == 3 && preview.newCount == 1 &&
        preview.conflictCount == 2 && preview.resultCount == 3 && preview.renamedResultCount == 5 &&
        preview.conflicts == std::vector<std::wstring>{ L"attack", L"HEAL" }, "Import conflict preview is incorrect.");
    const auto kept = MergePresets(current, incoming, ImportStrategy::KeepExisting);
    Require(kept.size() == 3 && Same(kept[0], current[0]) && kept[2].notation == L"d8",
        "KeepExisting must retain the current or first incoming preset for each name.");
    const auto replaced = MergePresets(current, incoming, ImportStrategy::ReplaceExisting);
    Require(replaced.size() == 3 && replaced[0].notation == L"d6" && replaced[0].group == L"new" &&
        replaced[2].notation == L"d10", "ReplaceExisting must use the last incoming record and normalize names/groups.");
    const auto renamed = MergePresets(current, incoming, ImportStrategy::RenameIncoming);
    Require(renamed.size() == 5 && renamed[2].name == L"attack (3)" && renamed[4].name == L"HEAL (2)",
        "RenameIncoming must avoid existing suffixes and incoming name collisions.");
    Require(current[0].name == L"Attack" && incoming[0].name == L" attack ", "Import must not mutate input collections.");
    Throws([&] { MergePresets(current, incoming, static_cast<ImportStrategy>(99)); });
    Throws([&] { PreviewImport(incoming, {}); }); // An existing collection must already be unique.
    Throws([&] { ExportPresets(path, incoming); });
    Require(Read(path) == bytes, "Invalid export must preserve the existing file.");

    Preset longName{ std::wstring(59, L'x') + L"😀abc", L"d6" };
    const auto longRenamed = MergePresets({ longName }, { longName }, ImportStrategy::RenameIncoming);
    Require(longRenamed[1].name.size() <= MaxPresetName && !Utf8(longRenamed[1].name).empty(),
        "Renaming a maximum-length Unicode name must not split a surrogate pair.");

    std::vector<Preset> maximum;
    for (std::size_t i = 0; i < MaxPresets; ++i) maximum.push_back({ L"Preset " + std::to_wstring(i), L"d6" });
    const std::vector<Preset> conflict{ { L"Preset 0", L"d8" } };
    Require(PreviewImport(maximum, conflict).renamedResultCount == 51, "Preview must explain an over-limit rename strategy.");
    Throws([&] { MergePresets(maximum, conflict, ImportStrategy::RenameIncoming); });
    Require(MergePresets(maximum, conflict, ImportStrategy::KeepExisting).size() == MaxPresets &&
        MergePresets(maximum, conflict, ImportStrategy::ReplaceExisting)[0].notation == L"d8",
        "Keep/replace must remain available at the preset limit.");
    Throws([&] { MergePresets(maximum, { { L"New", L"d6" } }, ImportStrategy::KeepExisting); });
    const auto maximumPath = directory.path / L"maximum.drpresets";
    ExportPresets(maximumPath, maximum);
    Require(ReadPresets(maximumPath).size() == MaxPresets, "The 50 preset boundary must remain usable.");
    reject(Read(maximumPath) + "Extra\t\td6\t1\t0\t0\t0\t0\t0\n");

    const auto attributes = GetFileAttributesW(path.c_str());
    Require(attributes != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(path.c_str(), attributes | FILE_ATTRIBUTE_READONLY),
        "Could not protect the preset test fixture.");
    bool failed = false;
    try { ExportPresets(path, current); } catch (const std::exception&) { failed = true; }
    Require(SetFileAttributesW(path.c_str(), attributes) != 0, "Could not restore preset fixture attributes.");
    Require(failed && Read(path) == bytes, "A failed export replacement must preserve existing presets.");
    for (const auto& entry : fs::directory_iterator(directory.path))
        Require(entry.path().extension() != L".tmp", "Failed preset export left temporary files behind.");
    ExportPresets(path, current);
    Require(ReadPresets(path).size() == current.size(), "Export retry must succeed after the blocking condition is removed.");
    const auto emptyPath = directory.path / L"empty.drpresets";
    ExportPresets(emptyPath, {});
    Require(ReadPresets(emptyPath).empty(), "An empty preset collection must round-trip.");
}
