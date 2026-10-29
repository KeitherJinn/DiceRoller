#include "AppSettings.h"
#include "ResultStore.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <limits>

namespace dice {
namespace {
constexpr std::array<char, 8> Magic{ 'D', 'R', 'S', 'E', 'T', 'T', 'I', 'N' };
constexpr std::uint32_t FormatVersion = 2;
constexpr std::size_t MaxFileBytes = 256 * 1024;
constexpr std::size_t MaxNotation = 1024;
constexpr std::size_t MaxCsvPath = 32760;

[[noreturn]] void SettingsError(const char* action) {
    const auto code = GetLastError();
    throw std::runtime_error(std::string(action) + " (Windows error " + std::to_string(code) + ").");
}

class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value_; }
    void Close() {
        const auto value = value_;
        value_ = INVALID_HANDLE_VALUE;
        if (value != INVALID_HANDLE_VALUE && !CloseHandle(value)) SettingsError("Could not close settings");
    }
private:
    HANDLE value_;
};

void OneLine(const std::wstring& text, std::size_t maximum, bool emptyAllowed, const char* field) {
    if ((!emptyAllowed && Trim(text).empty()) || text.size() > maximum ||
        std::any_of(text.begin(), text.end(), [](wchar_t ch) { return ch < 32 || ch == 127; }))
        throw std::invalid_argument(std::string(field) + " has an invalid length or control character.");
    // A valid std::wstring can still contain an unpaired UTF-16 surrogate.
    Utf8(text);
}

void ValidateNotation(const std::wstring& notation, int times, Mode mode,
    int successThreshold, const Target& target) {
    OneLine(notation, MaxNotation, false, "Dice notation");
    Request request;
    request.name = L"settings";
    request.notation = notation;
    request.spec = Parse(notation);
    request.spec.successThreshold = successThreshold;
    request.times = times;
    request.mode = mode;
    request.target = target;
    Validate(request);
}

void U32(std::string& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<char>((value >> shift) & 0xff));
}

void String(std::string& bytes, const std::wstring& text) {
    const auto utf8 = Utf8(text);
    U32(bytes, static_cast<std::uint32_t>(utf8.size()));
    bytes += utf8;
}

void Rules(std::string& bytes, int successThreshold, const Target& target) {
    U32(bytes, static_cast<std::uint32_t>(successThreshold));
    U32(bytes, target.enabled ? 1 : 0);
    U32(bytes, static_cast<std::uint32_t>(target.comparison));
    const auto value = static_cast<std::uint64_t>(target.value);
    U32(bytes, static_cast<std::uint32_t>(value));
    U32(bytes, static_cast<std::uint32_t>(value >> 32));
}

class Reader {
public:
    explicit Reader(const std::string& bytes) : bytes_(bytes) {}
    std::uint32_t U32() {
        Need(4);
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[offset_++])) << shift;
        return value;
    }
    int Integer() {
        const auto value = U32();
        const std::int64_t signedValue = value <= 0x7fffffffU ? value : static_cast<std::int64_t>(value) - 0x100000000LL;
        return static_cast<int>(signedValue);
    }
    bool Boolean() {
        const auto value = U32();
        if (value > 1) throw std::runtime_error("Settings contain an invalid boolean value.");
        return value != 0;
    }
    Mode RollMode() {
        const auto value = U32();
        if (value > static_cast<std::uint32_t>(Mode::Disadvantage))
            throw std::runtime_error("Settings contain an invalid roll mode.");
        return static_cast<Mode>(value);
    }
    Target ReadTarget() {
        Target target;
        target.enabled = Boolean();
        const auto comparison = U32();
        if (comparison > static_cast<std::uint32_t>(Compare::Less))
            throw std::runtime_error("Settings contain an invalid target comparison.");
        target.comparison = static_cast<Compare>(comparison);
        const auto low = static_cast<std::uint64_t>(U32());
        const auto value = low | (static_cast<std::uint64_t>(U32()) << 32);
        target.value = value <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
            ? static_cast<std::int64_t>(value) : -static_cast<std::int64_t>(~value) - 1;
        return target;
    }
    std::wstring String(std::size_t maximum) {
        const auto length = U32();
        if (length > maximum * 4) throw std::runtime_error("A settings field is too long.");
        Need(length);
        auto result = Wide(bytes_.substr(offset_, length));
        offset_ += length;
        if (result.size() > maximum) throw std::runtime_error("A settings field is too long.");
        return result;
    }
    std::uint32_t Header() {
        Need(Magic.size());
        if (!std::equal(Magic.begin(), Magic.end(), bytes_.begin()))
            throw std::runtime_error("Settings have an invalid header.");
        offset_ += Magic.size();
        const auto version = U32();
        if (version != 1 && version != FormatVersion)
            throw std::runtime_error("Settings use an unsupported format version.");
        return version;
    }
    void End() const {
        if (offset_ != bytes_.size()) throw std::runtime_error("Settings contain unexpected trailing data.");
    }
private:
    void Need(std::size_t amount) const {
        if (amount > bytes_.size() - offset_) throw std::runtime_error("Settings are incomplete or damaged.");
    }
    const std::string& bytes_;
    std::size_t offset_ = 0;
};

std::string Serialize(const AppSettings& settings) {
    ValidateSettings(settings);
    std::string bytes(Magic.begin(), Magic.end());
    U32(bytes, FormatVersion);
    String(bytes, settings.name);
    String(bytes, settings.notation);
    String(bytes, settings.csvPath);
    U32(bytes, settings.times);
    U32(bytes, static_cast<std::uint32_t>(settings.mode));
    U32(bytes, settings.autoSave ? 1 : 0);
    U32(bytes, settings.showTimestamp ? 1 : 0);
    U32(bytes, settings.window.valid ? 1 : 0);
    U32(bytes, static_cast<std::uint32_t>(settings.window.x));
    U32(bytes, static_cast<std::uint32_t>(settings.window.y));
    U32(bytes, settings.window.width);
    U32(bytes, settings.window.height);
    U32(bytes, settings.window.maximized ? 1 : 0);
    U32(bytes, static_cast<std::uint32_t>(settings.presets.size()));
    for (const auto& preset : settings.presets) {
        String(bytes, preset.name);
        String(bytes, preset.notation);
        U32(bytes, preset.times);
        U32(bytes, static_cast<std::uint32_t>(preset.mode));
    }
    // Keep the v1 prefix stable. Version 2 appends rule options and group metadata.
    Rules(bytes, settings.successThreshold, settings.target);
    for (const auto& preset : settings.presets) {
        String(bytes, preset.group);
        Rules(bytes, preset.successThreshold, preset.target);
    }
    if (bytes.size() > MaxFileBytes) throw std::length_error("Settings exceed the supported file size.");
    return bytes;
}

AppSettings Deserialize(const std::string& bytes) {
    Reader input(bytes);
    const auto version = input.Header();
    AppSettings result;
    result.name = input.String(128);
    result.notation = input.String(MaxNotation);
    result.csvPath = input.String(MaxCsvPath);
    result.times = input.Integer();
    result.mode = input.RollMode();
    result.autoSave = input.Boolean();
    result.showTimestamp = input.Boolean();
    result.window.valid = input.Boolean();
    result.window.x = input.Integer();
    result.window.y = input.Integer();
    result.window.width = input.Integer();
    result.window.height = input.Integer();
    result.window.maximized = input.Boolean();
    const auto count = input.U32();
    if (count > MaxPresets) throw std::runtime_error("Settings contain too many presets.");
    for (std::uint32_t i = 0; i < count; ++i) {
        Preset preset;
        preset.name = input.String(MaxPresetName);
        preset.notation = input.String(MaxNotation);
        preset.times = input.Integer();
        preset.mode = input.RollMode();
        result.presets.push_back(std::move(preset));
    }
    if (version >= 2) {
        result.successThreshold = input.Integer();
        result.target = input.ReadTarget();
        for (auto& preset : result.presets) {
            preset.group = input.String(MaxPresetGroup);
            preset.successThreshold = input.Integer();
            preset.target = input.ReadTarget();
        }
    }
    input.End();
    ValidateSettings(result);
    return result;
}
}

AppSettings DefaultSettings() {
    AppSettings settings;
    settings.csvPath = (DefaultDirectory() / L"dice_results_v3.csv").wstring();
    return settings;
}

std::filesystem::path DefaultSettingsPath() {
    return DefaultDirectory().parent_path() / L"settings.bin";
}

void ValidatePreset(const Preset& preset) {
    OneLine(preset.name, MaxPresetName, false, "Preset name");
    OneLine(preset.group, MaxPresetGroup, true, "Preset group");
    ValidateNotation(preset.notation, preset.times, preset.mode, preset.successThreshold, preset.target);
}

void ValidateSettings(const AppSettings& settings) {
    OneLine(settings.name, 128, false, "Roll name");
    ValidateNotation(settings.notation, settings.times, settings.mode, settings.successThreshold, settings.target);
    OneLine(settings.csvPath, MaxCsvPath, true, "CSV path");
    if (!settings.csvPath.empty()) ResolvePath(settings.csvPath, std::filesystem::path(L"C:\\"));
    const auto& window = settings.window;
    if (window.x < -1000000 || window.x > 1000000 || window.y < -1000000 || window.y > 1000000 ||
        window.width < 160 || window.width > 16384 || window.height < 160 || window.height > 16384)
        throw std::invalid_argument("The remembered window placement is out of range.");
    if (settings.presets.size() > MaxPresets) throw std::invalid_argument("At most 50 custom presets can be saved.");
    for (std::size_t i = 0; i < settings.presets.size(); ++i) {
        ValidatePreset(settings.presets[i]);
        const auto name = Trim(settings.presets[i].name);
        for (std::size_t j = 0; j < i; ++j) {
            const auto previous = Trim(settings.presets[j].name);
            if (CompareStringOrdinal(name.c_str(), static_cast<int>(name.size()), previous.c_str(),
                static_cast<int>(previous.size()), TRUE) == CSTR_EQUAL)
                throw std::invalid_argument("Custom preset names must be unique (ignoring case).");
        }
    }
}

SettingsLoad LoadSettings(const std::filesystem::path& path, const AppSettings& defaults) {
    SettingsLoad result{ defaults, {}, false };
    try {
        const auto raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (raw == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return result;
            SettingsError("Could not open settings");
        }
        Handle file(raw);
        result.found = true;
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file.Get(), &length)) SettingsError("Could not read settings size");
        if (length.QuadPart < 0 || length.QuadPart > static_cast<LONGLONG>(MaxFileBytes))
            throw std::runtime_error("Settings exceed the supported file size.");
        std::string bytes(static_cast<std::size_t>(length.QuadPart), '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD read = 0;
            if (!ReadFile(file.Get(), bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &read, nullptr))
                SettingsError("Could not read settings");
            if (!read) throw std::runtime_error("Settings are incomplete or damaged.");
            offset += read;
        }
        result.settings = Deserialize(bytes);
    } catch (const std::exception& error) {
        result.settings = defaults;
        result.warning = L"Saved settings could not be loaded; defaults are in use. ";
        try { result.warning += Wide(error.what()); }
        catch (...) { result.warning += L"The configuration is unreadable or invalid."; }
    }
    return result;
}

std::filesystem::path BackupSettings(const std::filesystem::path& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        SettingsError("Could not inspect settings before backup");
    }
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Could not create a settings backup filename.");
    wchar_t guid[40]{};
    if (!StringFromGUID2(id, guid, 40)) throw std::runtime_error("Could not format a settings backup filename.");
    auto backup = path;
    backup += std::wstring(L".") + guid + L".bak";
    // Copy the original bytes even if they cannot be parsed by this application.
    // failIfExists is mandatory: an existing backup belongs to its earlier writer.
    if (!CopyFileW(path.c_str(), backup.c_str(), TRUE)) SettingsError("Could not back up settings");
    return backup;
}

void SaveSettings(const std::filesystem::path& path, const AppSettings& settings) {
    const auto bytes = Serialize(settings);
    if (path.empty() || path.filename().empty()) throw std::invalid_argument("A settings file path is required.");
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Could not create a temporary settings filename.");
    wchar_t guid[40]{};
    if (!StringFromGUID2(id, guid, 40)) throw std::runtime_error("Could not format a temporary settings filename.");
    auto temporary = path;
    temporary += std::wstring(L".") + guid + L".tmp";
    const auto raw = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) SettingsError("Could not create temporary settings");
    // Cleanup owns only the unique file successfully created above.
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { if (!path.empty()) DeleteFileW(path.c_str()); }
    } cleanup{ std::move(temporary) };
    Handle file(raw);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(file.Get(), bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &written, nullptr))
            SettingsError("Could not write settings");
        if (!written) throw std::runtime_error("Settings write made no progress.");
        offset += written;
    }
    if (!FlushFileBuffers(file.Get())) SettingsError("Could not flush settings");
    file.Close();
    if (!MoveFileExW(cleanup.path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        SettingsError("Could not replace settings");
    cleanup.path.clear();
}
}
