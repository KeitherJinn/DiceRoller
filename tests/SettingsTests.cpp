#include "AppSettings.h"
#include "ResultStore.h"
#include <windows.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

namespace {
namespace fs = std::filesystem;
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void Throws(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "Invalid settings should have been rejected.");
}
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(static_cast<bool>(input), "Could not read settings test fixture.");
    return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
}
void Put(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    Require(!output.fail(), "Could not write settings test fixture.");
}
bool Equal(const dice::AppSettings& a, const dice::AppSettings& b) {
    if (a.name != b.name || a.notation != b.notation || a.csvPath != b.csvPath || a.times != b.times ||
        a.mode != b.mode || a.autoSave != b.autoSave || a.showTimestamp != b.showTimestamp ||
        a.window.valid != b.window.valid || a.window.x != b.window.x || a.window.y != b.window.y ||
        a.window.width != b.window.width || a.window.height != b.window.height ||
        a.window.maximized != b.window.maximized || a.presets.size() != b.presets.size() ||
        a.successThreshold != b.successThreshold || a.target.enabled != b.target.enabled ||
        a.target.comparison != b.target.comparison || a.target.value != b.target.value) return false;
    for (std::size_t i = 0; i < a.presets.size(); ++i) {
        if (a.presets[i].name != b.presets[i].name || a.presets[i].notation != b.presets[i].notation ||
            a.presets[i].times != b.presets[i].times || a.presets[i].mode != b.presets[i].mode ||
            a.presets[i].group != b.presets[i].group || a.presets[i].successThreshold != b.presets[i].successThreshold ||
            a.presets[i].target.enabled != b.presets[i].target.enabled ||
            a.presets[i].target.comparison != b.presets[i].target.comparison ||
            a.presets[i].target.value != b.presets[i].target.value) return false;
    }
    return true;
}
void SetU32(std::string& bytes, std::size_t offset, std::uint32_t value) {
    Require(offset + 4 <= bytes.size(), "Invalid mutation offset in settings test.");
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[offset++] = static_cast<char>((value >> shift) & 0xff);
}
std::size_t SkipString(const std::string& bytes, std::size_t offset) {
    Require(offset + 4 <= bytes.size(), "Invalid settings fixture field offset.");
    std::uint32_t count = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        count |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset++])) << shift;
    Require(offset + count <= bytes.size(), "Invalid settings fixture string size.");
    return offset + count;
}
// Construct the documented v1 layout independently from the production writer.
// A v2 writer must not accidentally turn this compatibility test into v2-only coverage.
std::string LegacySettings(const dice::AppSettings& settings) {
    std::string bytes = "DRSETTIN";
    auto number = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<char>((value >> shift) & 0xff));
    };
    auto text = [&](const std::wstring& value) {
        const auto encoded = dice::Utf8(value);
        number(static_cast<std::uint32_t>(encoded.size()));
        bytes += encoded;
    };
    number(1); text(settings.name); text(settings.notation); text(settings.csvPath);
    number(settings.times); number(static_cast<std::uint32_t>(settings.mode));
    number(settings.autoSave); number(settings.showTimestamp); number(settings.window.valid);
    number(static_cast<std::uint32_t>(settings.window.x)); number(static_cast<std::uint32_t>(settings.window.y));
    number(settings.window.width); number(settings.window.height); number(settings.window.maximized);
    number(static_cast<std::uint32_t>(settings.presets.size()));
    for (const auto& preset : settings.presets) {
        text(preset.name); text(preset.notation); number(preset.times); number(static_cast<std::uint32_t>(preset.mode));
    }
    return bytes;
}
}

void SettingsRegressionTests(const std::filesystem::path& base) {
    using namespace dice;
    const auto directory = base / L"settings-regression";
    fs::create_directories(directory);
    const auto path = directory / L"设置😀.bin";
    const auto damaged = directory / L"damaged.bin";
    AppSettings defaults;
    defaults.csvPath = (directory / L"default.csv").wstring();

    const auto missing = LoadSettings(directory / L"missing.bin", defaults);
    Require(!missing.found && missing.warning.empty() && Equal(missing.settings, defaults),
        "A missing configuration must use defaults silently.");
    Require(BackupSettings(directory / L"missing.bin").empty(), "Missing settings must not create a backup.");

    AppSettings original;
    original.name = L"中文掷骰 😀";
    original.notation = L"2d6+3";
    original.csvPath = (directory / L"结果目录" / L"战斗😀.csv").wstring();
    original.times = 7;
    original.mode = Mode::Advantage;
    original.autoSave = false;
    original.showTimestamp = true;
    original.window = { true, -3000, 140, 960, 720, true };
    original.presets = { { L"攻击 😀", L"d20+5", 2, Mode::Advantage },
                         { L"伤害", L"2d6+3", 1, Mode::Normal },
                         { L"劣势", L"d12", 3, Mode::Disadvantage } };
    original.target = { true, Compare::AtLeast, 8 };
    original.presets.front().group = L"角色 😀";
    original.presets.front().target = { true, Compare::Equal, 20 };
    original.presets.push_back({ L"骰池", L"8d10", 1, Mode::Normal, L"角色 B", 8,
        { true, Compare::AtLeast, 3 } });
    SaveSettings(path, original);
    const auto loaded = LoadSettings(path, defaults);
    Require(loaded.found && loaded.warning.empty() && Equal(loaded.settings, original),
        "Unicode settings, negative screen positions and presets must round-trip exactly.");
    const auto bytes = Read(path);
    Require(static_cast<unsigned char>(bytes[8]) == 2, "Settings must be written with the v2 schema.");
    const auto legacyPath = directory / L"legacy-v1.bin";
    const auto legacyBytes = LegacySettings(original);
    Put(legacyPath, legacyBytes);
    auto legacyExpected = original;
    legacyExpected.successThreshold = 0;
    legacyExpected.target = {};
    for (auto& preset : legacyExpected.presets) { preset.group.clear(); preset.successThreshold = 0; preset.target = {}; }
    const auto legacyLoaded = LoadSettings(legacyPath, defaults);
    Require(legacyLoaded.found && legacyLoaded.warning.empty() && Equal(legacyLoaded.settings, legacyExpected),
        "Version 1 settings must retain old fields and default all new rule/group fields.");
    Require(Read(legacyPath) == legacyBytes, "Loading v1 settings must not rewrite or back up the file.");
    SaveSettings(legacyPath, legacyLoaded.settings);
    Require(static_cast<unsigned char>(Read(legacyPath)[8]) == 2 && Equal(LoadSettings(legacyPath, defaults).settings, legacyExpected),
        "Saving loaded v1 settings must migrate to v2 without changing their preferences.");

    auto poolSettings = original;
    poolSettings.notation = L"8d10"; poolSettings.mode = Mode::Normal; poolSettings.successThreshold = 8;
    const auto rulesPath = directory / L"all-rules.bin";
    for (const auto comparison : { Compare::AtLeast, Compare::AtMost, Compare::Equal, Compare::Greater, Compare::Less }) {
        for (const auto value : { (std::numeric_limits<std::int64_t>::min)(), (std::numeric_limits<std::int64_t>::max)() }) {
            poolSettings.target = { true, comparison, value };
            SaveSettings(rulesPath, poolSettings);
            Require(Equal(LoadSettings(rulesPath, defaults).settings, poolSettings),
                "All target comparisons, 64-bit extremes and global pool settings must round-trip.");
        }
    }
    const auto firstBackup = BackupSettings(path);
    const auto secondBackup = BackupSettings(path);
    Require(firstBackup != secondBackup && firstBackup.parent_path() == directory && secondBackup.parent_path() == directory,
        "Every settings backup must have a unique name beside the original file.");
    Require(Read(path) == bytes && Read(firstBackup) == bytes && Read(secondBackup) == bytes,
        "Backing up settings must preserve the exact original bytes without replacing earlier backups.");

    auto rejectedLoad = [&](const std::string& bad) {
        Put(damaged, bad);
        const auto result = LoadSettings(damaged, defaults);
        Require(result.found && !result.warning.empty() && Equal(result.settings, defaults),
            "Invalid settings must return complete defaults with a warning.");
        Require(Read(damaged) == bad, "Loading invalid settings must not rewrite the original file.");
    };
    // Every incomplete prefix must fail safely, including header and UTF-8 boundaries.
    for (std::size_t cut = 0; cut < bytes.size(); ++cut) rejectedLoad(bytes.substr(0, cut));
    rejectedLoad(bytes + 'x');
    rejectedLoad(std::string(256 * 1024 + 1, 'x'));
    auto bad = bytes;
    bad[0] = '?';
    rejectedLoad(bad);
    bad = bytes;
    SetU32(bad, 8, 99);
    rejectedLoad(bad);
    bad = bytes;
    SetU32(bad, 12, (std::numeric_limits<std::uint32_t>::max)());
    rejectedLoad(bad);
    bad = bytes;
    bad[16] = static_cast<char>(0xff);
    rejectedLoad(bad);
    bad = bytes;
    bad[16] = '\0';
    rejectedLoad(bad);

    auto numeric = SkipString(bytes, 12);
    numeric = SkipString(bytes, numeric);
    numeric = SkipString(bytes, numeric);
    for (const auto mutation : { std::pair<std::size_t, std::uint32_t>{ 0, 0 }, // times
        { 4, 99 }, { 8, 2 }, { 12, 2 }, { 16, 2 }, { 20, 1000001 },
        { 28, 159 }, { 32, 16385 }, { 36, 2 }, { 40, 51 } }) {
        bad = bytes;
        SetU32(bad, numeric + mutation.first, mutation.second);
        rejectedLoad(bad);
    }
    // The v2 extension begins immediately after the stable v1 prefix.
    for (const auto mutation : { std::pair<std::size_t, std::uint32_t>{ 0, 0xffffffffU },
        { 4, 2 }, { 8, 99 }, { 20, 1000000 } }) {
        bad = bytes;
        SetU32(bad, legacyBytes.size() + mutation.first, mutation.second);
        rejectedLoad(bad);
    }
    const auto damagedBackup = BackupSettings(damaged);
    Require(Read(damagedBackup) == bad && Read(damaged) == bad,
        "An invalid configuration must be backed up verbatim without being parsed or rewritten.");

    auto rejectedSave = [&](const AppSettings& invalid) {
        Throws([&] { SaveSettings(path, invalid); });
        Require(Read(path) == bytes, "Validation failure must preserve the previous settings file.");
    };
    auto invalid = original;
    invalid.name.clear();
    rejectedSave(invalid);
    invalid = original;
    invalid.name = std::wstring(129, L'x');
    rejectedSave(invalid);
    invalid = original;
    invalid.notation = L"2d0";
    rejectedSave(invalid);
    invalid = original;
    invalid.notation = std::wstring(129, L'1');
    rejectedSave(invalid);
    invalid = original;
    invalid.times = MaxRolls + 1;
    rejectedSave(invalid);
    invalid = original;
    invalid.notation = L"1000d6";
    invalid.times = 501; // Advantage doubles the generated dice budget.
    rejectedSave(invalid);
    invalid = original;
    invalid.mode = static_cast<Mode>(99);
    rejectedSave(invalid);
    invalid = original;
    invalid.csvPath = L"data.txt";
    rejectedSave(invalid);
    invalid = original;
    invalid.csvPath = std::wstring(32761, L'x');
    rejectedSave(invalid);
    invalid = original;
    invalid.name = std::wstring(1, static_cast<wchar_t>(0xd800));
    rejectedSave(invalid);
    invalid = original;
    invalid.window.height = 0;
    rejectedSave(invalid);
    invalid = original;
    invalid.presets.front().name = std::wstring(65, L'x');
    rejectedSave(invalid);
    invalid = original;
    invalid.presets.front().name = L"name\nline";
    rejectedSave(invalid);
    invalid = original;
    invalid.presets.front().times = 0;
    rejectedSave(invalid);
    invalid = original;
    invalid.presets.front().group = std::wstring(MaxPresetGroup + 1, L'g');
    rejectedSave(invalid);
    invalid = original;
    invalid.presets.front().group = L"group\tother";
    rejectedSave(invalid);
    invalid = original;
    invalid.successThreshold = -1;
    rejectedSave(invalid);
    invalid = original;
    invalid.target.comparison = static_cast<Compare>(99);
    rejectedSave(invalid);
    invalid = original;
    invalid.presets = { { L"Attack", L"d20", 1, Mode::Normal },
                        { L" attack ", L"2d6", 1, Mode::Normal } };
    rejectedSave(invalid);
    invalid = original;
    invalid.presets.resize(MaxPresets + 1);
    rejectedSave(invalid);

    AppSettings maximum = original;
    maximum.csvPath.clear(); // Clearing the CSV path is a rememberable UI preference.
    maximum.presets.clear();
    for (std::size_t i = 0; i < MaxPresets; ++i)
        maximum.presets.push_back({ L"Preset " + std::to_wstring(i), L"d6", 1, Mode::Normal });
    maximum.presets.back().name = std::wstring(MaxPresetName, L'名');
    const auto maximumPath = directory / L"maximum.bin";
    SaveSettings(maximumPath, maximum);
    const auto maximumLoaded = LoadSettings(maximumPath, defaults);
    Require(maximumLoaded.warning.empty() && Equal(maximumLoaded.settings, maximum),
        "The documented preset/name limits and empty CSV paths must remain usable.");

    AppSettings changed = original;
    changed.name = L"new value";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    Require(attributes != INVALID_FILE_ATTRIBUTES, "Could not read settings fixture attributes.");
    Require(SetFileAttributesW(path.c_str(), attributes | FILE_ATTRIBUTE_READONLY) != 0,
        "Could not make settings fixture read-only.");
    bool saveFailed = false;
    try { SaveSettings(path, changed); } catch (const std::exception&) { saveFailed = true; }
    const bool restoredAttributes = SetFileAttributesW(path.c_str(), attributes) != 0;
    Require(restoredAttributes, "Could not restore settings fixture attributes.");
    Require(saveFailed && Read(path) == bytes, "A failed settings replacement must preserve the original bytes.");
    for (const auto& entry : fs::directory_iterator(directory))
        Require(entry.path().extension() != L".tmp", "A failed settings save left its temporary file behind.");

    const auto lock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(lock != INVALID_HANDLE_VALUE, "Could not exclusively lock settings fixture.");
    const auto unreadable = LoadSettings(path, defaults);
    bool backupFailed = false;
    try { BackupSettings(path); } catch (const std::exception&) { backupFailed = true; }
    saveFailed = false;
    try { SaveSettings(path, changed); } catch (const std::exception&) { saveFailed = true; }
    CloseHandle(lock);
    Require(!unreadable.warning.empty() && Equal(unreadable.settings, defaults),
        "Unreadable settings must allow startup with defaults.");
    Require(backupFailed && Read(path) == bytes, "An unreadable settings backup must fail without changing the source.");
    Require(saveFailed && Read(path) == bytes, "A locked settings file must survive a failed replacement.");
    SaveSettings(path, changed);
    Require(Equal(LoadSettings(path, defaults).settings, changed), "Retrying settings save must succeed after unlocking.");
    Require(Read(firstBackup) == bytes && Read(secondBackup) == bytes,
        "Saving a new configuration must preserve the earlier backup bytes.");
    Throws([&] { SaveSettings({}, changed); });
    const auto blocker = directory / L"not-a-directory";
    Put(blocker, "keep");
    Throws([&] { SaveSettings(blocker / L"settings.bin", changed); });
    Require(Read(blocker) == "keep", "A blocked parent path must remain untouched.");
}
