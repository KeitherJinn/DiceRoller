#include "PresetExchange.h"
#include "ResultStore.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>

namespace dice {
namespace {
constexpr std::string_view Header = "DiceRollerPresets\t1";
constexpr std::size_t MaxFileBytes = 256 * 1024;

[[noreturn]] void FileError(const char* action) {
    const auto error = GetLastError();
    throw std::runtime_error(std::string(action) + " (Windows error " + std::to_string(error) + ").");
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
        if (value != INVALID_HANDLE_VALUE && !CloseHandle(value)) FileError("Could not close preset file");
    }
private:
    HANDLE value_;
};

bool SameName(const std::wstring& left, const std::wstring& right) {
    const auto a = Trim(left), b = Trim(right);
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
        static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
std::size_t Find(const std::vector<Preset>& presets, const std::wstring& name) {
    for (std::size_t i = 0; i < presets.size(); ++i)
        if (SameName(presets[i].name, name)) return i;
    return presets.size();
}
void ValidateList(const std::vector<Preset>& presets, bool unique) {
    if (presets.size() > MaxPresets) throw std::invalid_argument("At most 50 custom presets are supported.");
    for (std::size_t i = 0; i < presets.size(); ++i) {
        ValidatePreset(presets[i]);
        if (unique && Find(presets, presets[i].name) != i)
            throw std::invalid_argument("Preset names must be unique, ignoring case and surrounding spaces.");
    }
}
Preset Normalize(Preset preset) {
    preset.name = Trim(preset.name);
    preset.group = Trim(preset.group);
    return preset;
}
std::wstring UniqueName(const std::wstring& original, const std::vector<Preset>& existing) {
    for (std::size_t number = 2; ; ++number) {
        const auto suffix = L" (" + std::to_wstring(number) + L")";
        auto base = original.substr(0, MaxPresetName - suffix.size());
        if (!base.empty() && base.back() >= 0xd800 && base.back() <= 0xdbff) base.pop_back();
        const auto candidate = base + suffix;
        if (Find(existing, candidate) == existing.size()) return candidate;
    }
}

std::string ReadBytes(const std::filesystem::path& path) {
    const auto raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) FileError("Could not open preset file");
    Handle file(raw);
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.Get(), &length)) FileError("Could not read preset file size");
    if (length.QuadPart < 0 || length.QuadPart > static_cast<LONGLONG>(MaxFileBytes))
        throw std::runtime_error("The preset file exceeds the supported size.");
    std::string bytes(static_cast<std::size_t>(length.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        if (!ReadFile(file.Get(), bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &count, nullptr))
            FileError("Could not read preset file");
        if (!count) throw std::runtime_error("The preset file is incomplete.");
        offset += count;
    }
    return bytes;
}
void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
    if (path.empty() || path.filename().empty()) throw std::invalid_argument("A preset file path is required.");
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Could not create a temporary preset filename.");
    wchar_t guid[40]{};
    if (!StringFromGUID2(id, guid, 40)) throw std::runtime_error("Could not format a temporary preset filename.");
    auto temporary = path;
    temporary += std::wstring(L".") + guid + L".tmp";
    const auto raw = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) FileError("Could not create temporary preset file");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { if (!path.empty()) DeleteFileW(path.c_str()); }
    } cleanup{ std::move(temporary) };
    Handle file(raw);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = 0;
        if (!WriteFile(file.Get(), bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &count, nullptr))
            FileError("Could not write preset file");
        if (!count) throw std::runtime_error("Preset export made no progress.");
        offset += count;
    }
    if (!FlushFileBuffers(file.Get())) FileError("Could not flush preset file");
    file.Close();
    if (!MoveFileExW(cleanup.path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        FileError("Could not replace preset file");
    cleanup.path.clear();
}

template<class Integer> Integer Number(std::string_view field) {
    Integer value{};
    const auto parsed = std::from_chars(field.data(), field.data() + field.size(), value);
    if (field.empty() || parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size())
        throw std::runtime_error("The preset file contains an invalid integer.");
    return value;
}
std::array<std::string_view, 9> Fields(std::string_view line) {
    std::array<std::string_view, 9> fields;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto separator = line.find('\t');
        if ((i + 1 == fields.size()) != (separator == std::string_view::npos))
            throw std::runtime_error("The preset file contains a record with an incorrect field count.");
        fields[i] = line.substr(0, separator);
        if (separator != std::string_view::npos) line.remove_prefix(separator + 1);
    }
    return fields;
}
std::string_view Line(std::string_view& remaining) {
    const auto end = remaining.find('\n');
    auto line = remaining.substr(0, end);
    remaining.remove_prefix(end == std::string_view::npos ? remaining.size() : end + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
}
}

void ExportPresets(const std::filesystem::path& path, const std::vector<Preset>& presets) {
    ValidateList(presets, true);
    std::string bytes = std::string(Header) + "\r\n";
    for (const auto& preset : presets) {
        bytes += Utf8(preset.name) + '\t' + Utf8(preset.group) + '\t' + Utf8(preset.notation) + '\t'
            + std::to_string(preset.times) + '\t' + std::to_string(static_cast<int>(preset.mode)) + '\t'
            + std::to_string(preset.successThreshold) + '\t' + (preset.target.enabled ? "1" : "0") + '\t'
            + std::to_string(static_cast<int>(preset.target.comparison)) + '\t' + std::to_string(preset.target.value) + "\r\n";
    }
    if (bytes.size() > MaxFileBytes) throw std::length_error("Preset export exceeds the supported size.");
    WriteBytes(path, bytes);
}

std::vector<Preset> ReadPresets(const std::filesystem::path& path) {
    const auto bytes = ReadBytes(path);
    std::string_view remaining(bytes);
    if (remaining.substr(0, 3) == "\xEF\xBB\xBF") remaining.remove_prefix(3);
    if (Line(remaining) != Header) throw std::runtime_error("The preset file has an invalid header or unsupported version.");
    std::vector<Preset> presets;
    while (!remaining.empty()) {
        if (presets.size() == MaxPresets) throw std::runtime_error("The preset file contains more than 50 presets.");
        const auto fields = Fields(Line(remaining));
        Preset preset;
        preset.name = Wide(std::string(fields[0]));
        preset.group = Wide(std::string(fields[1]));
        preset.notation = Wide(std::string(fields[2]));
        preset.times = Number<int>(fields[3]);
        const auto mode = Number<int>(fields[4]);
        if (mode < 0 || mode > static_cast<int>(Mode::Disadvantage)) throw std::runtime_error("Invalid imported roll mode.");
        preset.mode = static_cast<Mode>(mode);
        preset.successThreshold = Number<int>(fields[5]);
        const auto enabled = Number<int>(fields[6]);
        if (enabled != 0 && enabled != 1) throw std::runtime_error("Invalid imported target state.");
        preset.target.enabled = enabled != 0;
        const auto comparison = Number<int>(fields[7]);
        if (comparison < 0 || comparison > static_cast<int>(Compare::Less)) throw std::runtime_error("Invalid imported target comparison.");
        preset.target.comparison = static_cast<Compare>(comparison);
        preset.target.value = Number<std::int64_t>(fields[8]);
        ValidatePreset(preset);
        presets.push_back(Normalize(std::move(preset)));
    }
    return presets;
}

ImportPreview PreviewImport(const std::vector<Preset>& current, const std::vector<Preset>& incoming) {
    ValidateList(current, true);
    ValidateList(incoming, false);
    ImportPreview preview;
    preview.existingCount = current.size();
    preview.incomingCount = incoming.size();
    auto names = current;
    for (const auto& preset : incoming) {
        if (Find(names, preset.name) != names.size()) {
            ++preview.conflictCount;
            preview.conflicts.push_back(Trim(preset.name));
        } else {
            ++preview.newCount;
            names.push_back(preset);
        }
    }
    preview.resultCount = current.size() + preview.newCount;
    preview.renamedResultCount = current.size() + incoming.size();
    return preview;
}

std::vector<Preset> MergePresets(const std::vector<Preset>& current,
    const std::vector<Preset>& incoming, ImportStrategy strategy) {
    const auto preview = PreviewImport(current, incoming);
    if (strategy != ImportStrategy::KeepExisting && strategy != ImportStrategy::RenameIncoming && strategy != ImportStrategy::ReplaceExisting)
        throw std::invalid_argument("Unknown preset import strategy.");
    const auto resultCount = strategy == ImportStrategy::RenameIncoming ? preview.renamedResultCount : preview.resultCount;
    if (resultCount > MaxPresets) throw std::invalid_argument("This import would exceed 50 presets. Choose another strategy or remove some presets first.");
    auto merged = current;
    for (const auto& original : incoming) {
        auto preset = Normalize(original);
        const auto index = Find(merged, preset.name);
        if (index == merged.size()) merged.push_back(std::move(preset));
        else if (strategy == ImportStrategy::ReplaceExisting) merged[index] = std::move(preset);
        else if (strategy == ImportStrategy::RenameIncoming) {
            preset.name = UniqueName(preset.name, merged);
            merged.push_back(std::move(preset));
        }
    }
    ValidateList(merged, true);
    return merged;
}
}
