#include "HistoryStore.h"
#include "ResultStore.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

namespace dice {
namespace {
constexpr std::size_t MaxRecordBytes = 32 * 1024 * 1024;
constexpr std::size_t MaxLoadBytes = 128 * 1024 * 1024;
constexpr std::size_t MaxRecords = 10000;
constexpr std::uint64_t MaxCsvBytes = 512ULL * 1024 * 1024;
constexpr std::size_t MaxLineBytes = 128 * 1024;
constexpr char Magic[] = "DRHIST01";

[[noreturn]] void IoError(const char* action) {
    const auto code = GetLastError();
    throw std::runtime_error(std::string(action) + " (Windows error " + std::to_string(code) + ").");
}
void CheckCancel(const CancelCheck& check) { if (check && check()) throw Cancelled(); }
class Handle {
public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    ~Handle() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return handle_; }
    void Close() {
        const auto h = handle_; handle_ = INVALID_HANDLE_VALUE;
        if (h != INVALID_HANDLE_VALUE && !CloseHandle(h)) IoError("Could not close history file");
    }
private:
    HANDLE handle_;
};
std::string NewId() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Could not create a history ID.");
    wchar_t text[40]{};
    if (!StringFromGUID2(id, text, 40)) throw std::runtime_error("Could not format a history ID.");
    return Utf8(std::wstring(text + 1, 36));
}
bool ValidId(const std::string& id) {
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
        else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'A' && id[i] <= 'F') ||
                   (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}
std::string UtcNow() {
    SYSTEMTIME now{}; GetSystemTime(&now);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return text;
}
bool ValidDate(const std::string& value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (std::size_t i = 0; i < value.size(); ++i)
        if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9')) return false;
    const int year = std::stoi(value.substr(0, 4)), month = std::stoi(value.substr(5, 2)), day = std::stoi(value.substr(8, 2));
    if (year < 1601 || month < 1 || month > 12) return false;
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int maximum = days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    return day >= 1 && day <= maximum;
}
bool ValidCreated(const std::string& text) {
    if (text.size() != 24 || !ValidDate(text.substr(0, 10)) || text[10] != 'T' || text[13] != ':' ||
        text[16] != ':' || text[19] != '.' || text[23] != 'Z') return false;
    for (const int i : {11, 12, 14, 15, 17, 18, 20, 21, 22}) if (text[i] < '0' || text[i] > '9') return false;
    return std::stoi(text.substr(11, 2)) < 24 && std::stoi(text.substr(14, 2)) < 60 && std::stoi(text.substr(17, 2)) < 60;
}
void TextField(const std::wstring& text, std::size_t limit, bool allowEmpty) {
    if ((!allowEmpty && Trim(text).empty()) || text.size() > limit ||
        std::any_of(text.begin(), text.end(), [](wchar_t c) { return c < 32 || c == 127; }))
        throw std::runtime_error("History contains an invalid text field.");
    Utf8(text);
}
void U32(std::string& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xff));
}
void I64(std::string& out, std::int64_t value) {
    const auto bits = static_cast<std::uint64_t>(value);
    for (unsigned shift = 0; shift < 64; shift += 8) out.push_back(static_cast<char>((bits >> shift) & 0xff));
}
void String(std::string& out, const std::string& value) { U32(out, static_cast<std::uint32_t>(value.size())); out += value; }
void String(std::string& out, const std::wstring& value) { String(out, Utf8(value)); }
class Reader {
public:
    explicit Reader(const std::string& bytes) : bytes_(bytes) {}
    void Need(std::size_t count) const { if (count > bytes_.size() - offset_) throw std::runtime_error("History record is truncated."); }
    std::uint32_t U32() {
        Need(4); std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes_[offset_++])) << shift;
        return value;
    }
    int Int() { const auto value = U32(); return static_cast<int>(value <= 0x7fffffffU ? value : static_cast<std::int64_t>(value) - 0x100000000LL); }
    std::int64_t I64() {
        Need(8); std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes_[offset_++])) << shift;
        return value <= 0x7fffffffffffffffULL ? static_cast<std::int64_t>(value) : -1 - static_cast<std::int64_t>(~value);
    }
    bool Bool() { const auto value = U32(); if (value > 1) throw std::runtime_error("History contains an invalid boolean."); return value != 0; }
    std::size_t Count(std::size_t limit) { const auto value = U32(); if (value > limit) throw std::runtime_error("History field exceeds its limit."); return value; }
    std::string String(std::size_t limit) { const auto size = Count(limit); Need(size); auto text = bytes_.substr(offset_, size); offset_ += size; return text; }
    std::wstring WideString(std::size_t limit) { auto value = Wide(String(limit * 4)); if (value.size() > limit) throw std::runtime_error("History text exceeds its limit."); return value; }
    void Header() {
        Need(8);
        if (bytes_.compare(0, 8, Magic) != 0) throw std::runtime_error("History record has an invalid header.");
        offset_ = 8;
        if (U32() != 1) throw std::runtime_error("History record uses an unsupported version.");
    }
    void End() const { if (offset_ != bytes_.size()) throw std::runtime_error("History contains unexpected trailing bytes."); }
private:
    const std::string& bytes_;
    std::size_t offset_ = 0;
};
void WriteGroup(std::string& bytes, const Group& group) {
    U32(bytes, static_cast<std::uint32_t>(group.dice.size())); I64(bytes, group.total);
    for (std::size_t i = 0; i < group.dice.size(); ++i) { U32(bytes, group.dice[i]); U32(bytes, group.sides[i]); U32(bytes, group.kept[i] ? 1 : 0); }
}
Group ReadGroup(Reader& input, std::size_t& remaining) {
    Group group; const auto count = input.Count(MaxDice);
    if (count > remaining) throw std::runtime_error("History contains too many generated dice.");
    remaining -= count; group.total = input.I64();
    for (std::size_t i = 0; i < count; ++i) { group.dice.push_back(input.Int()); group.sides.push_back(input.Int()); group.kept.push_back(input.Bool()); }
    return group;
}
std::string Serialize(const HistoryEntry& entry, std::size_t count, bool saveState) {
    std::string bytes(Magic, 8); U32(bytes, 1);
    String(bytes, entry.id); String(bytes, entry.created); U32(bytes, saveState && entry.saved ? 1 : 0);
    String(bytes, saveState ? entry.exportedPath : std::wstring{});
    const auto& request = entry.batch.request; const auto& spec = request.spec;
    String(bytes, request.name); String(bytes, request.notation);
    U32(bytes, spec.count); U32(bytes, spec.sides); I64(bytes, spec.modifier);
    U32(bytes, static_cast<std::uint32_t>(spec.terms.size()));
    for (const auto& term : spec.terms) { U32(bytes, term.count); U32(bytes, term.sides); U32(bytes, term.keep); U32(bytes, term.keepHighest ? 1 : 0); }
    U32(bytes, spec.successThreshold); U32(bytes, static_cast<std::uint32_t>(request.mode)); U32(bytes, request.times);
    U32(bytes, request.showTimestamp ? 1 : 0); U32(bytes, request.target.enabled ? 1 : 0);
    U32(bytes, static_cast<std::uint32_t>(request.target.comparison)); I64(bytes, request.target.value);
    U32(bytes, static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const auto& result = entry.batch.results[i]; WriteGroup(bytes, result.first);
        U32(bytes, result.second ? 1 : 0); if (result.second) WriteGroup(bytes, *result.second);
        I64(bytes, result.chosen); I64(bytes, result.finalValue); String(bytes, result.timestamp);
    }
    if (bytes.size() > MaxRecordBytes) throw std::length_error("History record is too large.");
    return bytes;
}
HistoryEntry Deserialize(const std::string& bytes) {
    Reader input(bytes); input.Header(); HistoryEntry entry;
    entry.id = input.String(36); entry.created = input.String(24); entry.saved = input.Bool(); entry.exportedPath = input.WideString(32760);
    auto& request = entry.batch.request; auto& spec = request.spec;
    request.name = input.WideString(128); request.notation = input.WideString(1024);
    spec.count = input.Int(); spec.sides = input.Int(); spec.modifier = input.I64();
    const auto terms = input.Count(MaxDice);
    for (std::size_t i = 0; i < terms; ++i) { Term term; term.count = input.Int(); term.sides = input.Int(); term.keep = input.Int(); term.keepHighest = input.Bool(); spec.terms.push_back(term); }
    spec.successThreshold = input.Int(); request.mode = static_cast<Mode>(input.U32()); request.times = input.Int();
    request.showTimestamp = input.Bool(); request.target.enabled = input.Bool();
    request.target.comparison = static_cast<Compare>(input.U32()); request.target.value = input.I64();
    const auto count = input.Count(MaxRolls); std::size_t remaining = static_cast<std::size_t>(MaxWork);
    for (std::size_t i = 0; i < count; ++i) {
        Result result; result.first = ReadGroup(input, remaining); if (input.Bool()) result.second = ReadGroup(input, remaining);
        result.chosen = input.I64(); result.finalValue = input.I64(); result.timestamp = input.String(64); entry.batch.results.push_back(std::move(result));
    }
    input.End(); ValidateHistoryEntry(entry); return entry;
}
std::string ReadBytes(HANDLE file, std::size_t limit) {
    LARGE_INTEGER size{}; if (!GetFileSizeEx(file, &size)) IoError("Could not read history size");
    if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > limit) throw std::runtime_error("History file exceeds its size limit.");
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0'); std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        if (!ReadFile(file, bytes.data() + offset, static_cast<DWORD>((std::min)(bytes.size() - offset, std::size_t(65536))), &read, nullptr)) IoError("Could not read history");
        if (!read) throw std::runtime_error("History file is incomplete."); offset += read;
    }
    return bytes;
}
void Write(HANDLE file, const std::string& bytes, const CancelCheck& cancel = {}) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        CheckCancel(cancel); DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, static_cast<DWORD>((std::min)(bytes.size() - offset, std::size_t(65536))), &written, nullptr)) IoError("Could not write history/export");
        if (!written) throw std::runtime_error("File write made no progress."); offset += written;
    }
}
class Lock {
public:
    explicit Lock(const std::filesystem::path& path) : file_(Open(path)) {}
private:
    static HANDLE Open(const std::filesystem::path& path) {
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        auto companion = path; companion += L".lock";
        const auto handle = CreateFileW(companion.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (handle == INVALID_HANDLE_VALUE) IoError("File is locked, or its .lock companion already exists");
        return handle;
    }
    Handle file_;
};
class StagedFile {
public:
    explicit StagedFile(const std::filesystem::path& destination) : destination_(destination), temporary_(destination) {
        temporary_ += L"." + Wide(NewId()) + L".tmp";
        file_ = CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) IoError("Could not create a temporary history/export file");
    }
    ~StagedFile() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); if (!committed_) DeleteFileW(temporary_.c_str()); }
    StagedFile(const StagedFile&) = delete;
    StagedFile& operator=(const StagedFile&) = delete;
    HANDLE Get() const { return file_; }
    void Commit(bool replace, const CancelCheck& cancel = {}) {
        CheckCancel(cancel);
        if (!FlushFileBuffers(file_)) IoError("Could not flush history/export to disk");
        const auto handle = file_; file_ = INVALID_HANDLE_VALUE;
        if (!CloseHandle(handle)) IoError("Could not close history/export");
        CheckCancel(cancel);
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)))
            IoError("Could not commit history/export; original file preserved");
        committed_ = true;
    }
private:
    std::filesystem::path destination_, temporary_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    bool committed_ = false;
};
bool SameGroup(const Group& left, const Group& right) {
    return left.dice == right.dice && left.sides == right.sides && left.kept == right.kept && left.total == right.total;
}
std::filesystem::path EntryPath(const std::filesystem::path& root, const std::string& id) {
    if (!ValidId(id)) throw std::runtime_error("Invalid history ID.");
    return root / (Wide(id) + L".dhist");
}
bool Contains(const std::wstring& text, const std::wstring& find) {
    if (find.empty()) return true;
    if (find.size() > text.size()) return false;
    for (std::size_t i = 0; i <= text.size() - find.size(); ++i)
        if (CompareStringOrdinal(text.data() + i, static_cast<int>(find.size()), find.data(), static_cast<int>(find.size()), TRUE) == CSTR_EQUAL) return true;
    return false;
}
std::string Join(const std::vector<int>& values) {
    std::string text;
    for (const auto value : values) { if (!text.empty()) text += ';'; text += std::to_string(value); }
    return text;
}
std::string Kept(const std::vector<bool>& values) {
    std::string text;
    for (const auto value : values) { if (!text.empty()) text += ';'; text += value ? '1' : '0'; }
    return text;
}
std::string Comparison(Compare value) {
    switch (value) {
    case Compare::AtLeast: return ">="; case Compare::AtMost: return "<="; case Compare::Equal: return "=";
    case Compare::Greater: return ">"; case Compare::Less: return "<";
    }
    throw std::runtime_error("Invalid target comparison.");
}
bool Meets(const Target& target, std::int64_t value) {
    switch (target.comparison) {
    case Compare::AtLeast: return value >= target.value; case Compare::AtMost: return value <= target.value;
    case Compare::Equal: return value == target.value; case Compare::Greater: return value > target.value; case Compare::Less: return value < target.value;
    }
    throw std::runtime_error("Invalid target comparison.");
}
std::vector<std::string> CsvFields(const std::string& line) {
    std::vector<std::string> fields; std::size_t i = 0;
    for (;;) {
        std::string field;
        if (i < line.size() && line[i] == '"') {
            ++i; bool closed = false;
            while (i < line.size()) {
                if (line[i] == '"') {
                    if (i + 1 < line.size() && line[i + 1] == '"') { field += '"'; i += 2; }
                    else { ++i; closed = true; break; }
                } else field += line[i++];
            }
            if (!closed) throw std::runtime_error("CSV has an incomplete quoted field. Choose a new export file.");
        } else {
            while (i < line.size() && line[i] != ',') { if (line[i] == '"') throw std::runtime_error("CSV contains invalid quoting."); field += line[i++]; }
        }
        fields.push_back(std::move(field));
        if (fields.size() > 25) throw std::runtime_error("CSV contains too many columns.");
        if (i == line.size()) break;
        if (line[i++] != ',') throw std::runtime_error("CSV contains invalid field delimiters.");
    }
    return fields;
}
std::size_t Positive(const std::string& text) {
    if (text.empty() || text.size() > 5) throw std::runtime_error("CSV contains an invalid result index/count.");
    std::size_t value = 0;
    for (const char c : text) { if (c < '0' || c > '9') throw std::runtime_error("CSV contains an invalid result index/count."); value = value * 10 + c - '0'; }
    if (!value || value > MaxRolls) throw std::runtime_error("CSV contains an invalid result index/count.");
    return value;
}
class Lines {
public:
    explicit Lines(HANDLE file) : file_(file) {}
    bool Next(std::string& line) {
        line.clear();
        for (;;) {
            if (position_ == used_) {
                DWORD read = 0;
                if (!ReadFile(file_, buffer_.data(), static_cast<DWORD>(buffer_.size()), &read, nullptr)) IoError("Could not read the existing export");
                position_ = 0; used_ = read;
                if (!read) { if (!line.empty()) throw std::runtime_error("CSV has an incomplete final line. Choose a new export file."); return false; }
            }
            const char ch = buffer_[position_++];
            if (ch == '\n') { if (!line.empty() && line.back() == '\r') line.pop_back(); return true; }
            if (line.size() >= MaxLineBytes) throw std::runtime_error("CSV line exceeds its supported size.");
            line += ch;
        }
    }
private:
    HANDLE file_; std::array<char, 65536> buffer_{}; std::size_t position_ = 0, used_ = 0;
};
}

std::filesystem::path DefaultHistoryDirectory() { return DefaultDirectory().parent_path() / L"History"; }
HistoryEntry NewHistoryEntry(Batch batch) {
    HistoryEntry entry{NewId(), UtcNow(), std::move(batch), false, {}};
    ValidateHistoryEntry(entry); return entry;
}
void ValidateHistoryEntry(const HistoryEntry& entry) {
    if (!ValidId(entry.id) || !ValidCreated(entry.created)) throw std::runtime_error("History has an invalid ID or creation time.");
    const auto& request = entry.batch.request; Validate(request);
    TextField(request.name, 128, false); TextField(request.notation, 1024, false); TextField(entry.exportedPath, 32760, !entry.saved);
    if (request.target.comparison < Compare::AtLeast || request.target.comparison > Compare::Less) throw std::runtime_error("History has an invalid target comparison.");
    auto parsed = Parse(request.notation); const auto& spec = request.spec;
    if (parsed.count != spec.count || parsed.sides != spec.sides || parsed.modifier != spec.modifier || parsed.terms.size() != spec.terms.size())
        throw std::runtime_error("History notation does not match its recorded dice rules.");
    for (std::size_t i = 0; i < spec.terms.size(); ++i) {
        const auto& a = parsed.terms[i]; const auto& b = spec.terms[i];
        if (a.count != b.count || a.sides != b.sides || a.keep != b.keep || a.keepHighest != b.keepHighest)
            throw std::runtime_error("History notation does not match its recorded dice terms.");
    }
    if (entry.batch.results.size() > static_cast<std::size_t>(request.times)) throw std::runtime_error("History has more results than requested.");
    for (const auto& result : entry.batch.results) {
        TextField(Wide(result.timestamp), 64, true);
        std::size_t used = 0;
        const auto firstCount = result.first.dice.size();
        const auto actual = Roll(spec, request.mode, [&](int) {
            const auto index = used++;
            if (index < firstCount) return result.first.dice[index];
            if (result.second && index - firstCount < result.second->dice.size()) return result.second->dice[index - firstCount];
            throw std::runtime_error("History has incomplete per-die results.");
        });
        const auto totalCount = firstCount + (result.second ? result.second->dice.size() : 0);
        if (used != totalCount || !SameGroup(actual.first, result.first) || actual.second.has_value() != result.second.has_value() ||
            (actual.second && !SameGroup(*actual.second, *result.second)) || actual.chosen != result.chosen || actual.finalValue != result.finalValue)
            throw std::runtime_error("History per-die details do not match its result totals.");
    }
}
void SaveHistoryEntry(const std::filesystem::path& root, const HistoryEntry& entry) {
    if (root.empty()) throw std::invalid_argument("A history directory is required.");
    ValidateHistoryEntry(entry); const auto path = EntryPath(root, entry.id); Lock lock(path);
    Handle original(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    const bool exists = original.Get() != INVALID_HANDLE_VALUE;
    if (!exists && GetLastError() != ERROR_FILE_NOT_FOUND) IoError("Could not read existing history");
    if (exists) {
        const auto previous = Deserialize(ReadBytes(original.Get(), MaxRecordBytes));
        const auto count = previous.batch.results.size();
        if (count > entry.batch.results.size() || (previous.saved && (!entry.saved || count != entry.batch.results.size())) ||
            Serialize(previous, count, false) != Serialize(entry, count, false))
            throw std::runtime_error("A history ID cannot replace different results. Existing history was preserved.");
    }
    const auto bytes = Serialize(entry, entry.batch.results.size(), true);
    StagedFile target(path); Write(target.Get(), bytes); original.Close(); target.Commit(exists);
}
HistoryLoad LoadHistory(const std::filesystem::path& root) {
    HistoryLoad result; std::error_code error;
    if (root.empty()) { result.warnings.push_back(L"A history directory is required."); return result; }
    if (!std::filesystem::exists(root, error)) { if (error) result.warnings.push_back(L"History folder could not be read."); return result; }
    std::vector<std::filesystem::path> paths;
    std::filesystem::directory_iterator iterator(root, error), end;
    if (error) { result.warnings.push_back(L"History folder could not be read."); return result; }
    while (iterator != end) {
        if (iterator->path().extension() == L".dhist") {
            if (paths.size() == MaxRecords) { result.warnings.push_back(L"History load limit reached; additional records remain on disk."); break; }
            paths.push_back(iterator->path());
        }
        iterator.increment(error); if (error) { result.warnings.push_back(L"Some history records could not be enumerated."); break; }
    }
    std::sort(paths.begin(), paths.end()); std::size_t total = 0;
    for (const auto& path : paths) {
        try {
            Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file.Get() == INVALID_HANDLE_VALUE) IoError("Could not open history record");
            const auto bytes = ReadBytes(file.Get(), (std::min)(MaxRecordBytes, MaxLoadBytes - total));
            total += bytes.size(); auto entry = Deserialize(bytes);
            if (path.stem().wstring() != Wide(entry.id)) throw std::runtime_error("History filename and record ID do not match.");
            result.entries.push_back(std::move(entry));
        } catch (const std::exception& ex) {
            std::wstring reason; try { reason = Wide(ex.what()); } catch (...) { reason = L"Unreadable or invalid record."; }
            result.warnings.push_back(path.filename().wstring() + L": " + reason + L" Original file retained.");
        }
    }
    std::sort(result.entries.begin(), result.entries.end(), [](const HistoryEntry& a, const HistoryEntry& b) { return a.created == b.created ? a.id < b.id : a.created < b.created; });
    return result;
}
std::vector<std::size_t> QueryHistory(const std::vector<HistoryEntry>& entries, const HistoryQuery& query) {
    if ((!query.dateFrom.empty() && !ValidDate(query.dateFrom)) || (!query.dateTo.empty() && !ValidDate(query.dateTo)) ||
        (!query.dateFrom.empty() && !query.dateTo.empty() && query.dateFrom > query.dateTo))
        throw std::invalid_argument("Use UTC dates in YYYY-MM-DD order, with the start date no later than the end date.");
    std::vector<std::size_t> matches;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i]; const auto date = entry.created.substr(0, 10);
        if ((!query.unsavedOnly || !entry.saved) && Contains(entry.batch.request.name, Trim(query.name)) &&
            Contains(entry.batch.request.notation, Trim(query.notation)) && (query.dateFrom.empty() || date >= query.dateFrom) &&
            (query.dateTo.empty() || date <= query.dateTo)) matches.push_back(i);
    }
    return matches;
}
const std::string& HistoryCsvHeader() {
    static const std::string header = "BatchId,ResultIndex,BatchResultCount,BatchCreatedUtc,Name,DiceNotation,Mode,RequestedRolls,Modifier,PoolThreshold,TargetEnabled,TargetComparison,TargetValue,TargetMet,Roll1,Roll2,ChosenRoll,FinalResult,Timestamp,Roll1Dice,Roll1Sides,Roll1Kept,Roll2Dice,Roll2Sides,Roll2Kept";
    return header;
}
std::string HistoryCsvRow(const HistoryEntry& entry, std::size_t index) {
    const auto& request = entry.batch.request; const auto& result = entry.batch.results.at(index);
    const std::vector<std::string> fields{entry.id, std::to_string(index + 1), std::to_string(entry.batch.results.size()), entry.created,
        Utf8(request.name), Utf8(request.notation), ModeName(request.mode), std::to_string(request.times), std::to_string(request.spec.modifier),
        std::to_string(request.spec.successThreshold), request.target.enabled ? "1" : "0", Comparison(request.target.comparison),
        std::to_string(request.target.value), request.target.enabled ? (Meets(request.target, result.finalValue) ? "1" : "0") : "",
        std::to_string(result.first.total), result.second ? std::to_string(result.second->total) : "", std::to_string(result.chosen),
        std::to_string(result.finalValue), result.timestamp, Join(result.first.dice), Join(result.first.sides), Kept(result.first.kept),
        result.second ? Join(result.second->dice) : "", result.second ? Join(result.second->sides) : "", result.second ? Kept(result.second->kept) : ""};
    std::string row;
    for (const auto& field : fields) { if (!row.empty()) row += ','; row += CsvField(field); }
    return row + "\r\n";
}
ExportSummary ExportHistoryCsv(const std::filesystem::path& path, const std::vector<HistoryEntry>& entries, const CancelCheck& cancelled) {
    if (entries.empty()) throw std::invalid_argument("Select at least one history batch to export.");
    CheckCancel(cancelled); std::map<std::string, const HistoryEntry*> wanted;
    for (const auto& entry : entries) {
        ValidateHistoryEntry(entry);
        if (entry.batch.results.empty()) throw std::invalid_argument("A batch has no completed results to export.");
        if (!wanted.emplace(entry.id, &entry).second) throw std::invalid_argument("The same batch was selected more than once.");
    }
    if (path.empty() || path.filename().empty()) throw std::invalid_argument("Choose an export filename.");
    Lock lock(path);
    Handle source(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    const bool exists = source.Get() != INVALID_HANDLE_VALUE;
    if (!exists && GetLastError() != ERROR_FILE_NOT_FOUND) IoError("Could not read existing export");
    LARGE_INTEGER length{};
    if (exists && !GetFileSizeEx(source.Get(), &length)) IoError("Could not read export size");
    if (length.QuadPart < 0 || static_cast<std::uint64_t>(length.QuadPart) > MaxCsvBytes) throw std::runtime_error("Export exceeds 512 MiB. Choose a new CSV file.");
    std::set<std::string> complete;
    if (length.QuadPart) {
        Lines lines(source.Get()); std::string line;
        if (!lines.Next(line)) throw std::runtime_error("CSV has no header.");
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        if (line != HistoryCsvHeader()) throw std::runtime_error("This CSV uses an older or different format. Choose a new v3 export filename; the existing file was preserved.");
        std::string current; std::size_t expected = 0, seen = 0;
        while (lines.Next(line)) {
            CheckCancel(cancelled); const auto fields = CsvFields(line);
            if (fields.size() != 25 || !ValidId(fields[0])) throw std::runtime_error("CSV has an invalid batch record. Choose a new export file.");
            const auto index = Positive(fields[1]), count = Positive(fields[2]);
            if (fields[0] != current) {
                if (!current.empty()) { if (seen != expected) throw std::runtime_error("CSV contains a partial batch. Choose a new export file."); complete.insert(current); }
                current = fields[0]; expected = count; seen = 0;
                if (complete.count(current)) throw std::runtime_error("CSV contains a repeated batch ID. Choose a new export file.");
            }
            if (count != expected || index != ++seen || seen > expected) throw std::runtime_error("CSV contains missing or duplicated result indices. Choose a new export file.");
            const auto found = wanted.find(current);
            if (found != wanted.end()) {
                if (count != found->second->batch.results.size() || HistoryCsvRow(*found->second, index - 1) != line + "\r\n")
                    throw std::runtime_error("This batch ID already has different or incomplete results in the CSV. Choose a new export file.");
            }
        }
        if (!current.empty()) { if (seen != expected) throw std::runtime_error("CSV contains a partial batch. Choose a new export file."); complete.insert(current); }
    }
    ExportSummary summary;
    for (const auto& entry : entries) if (complete.count(entry.id)) ++summary.skippedBatches; else { ++summary.appendedBatches; summary.appendedResults += entry.batch.results.size(); }
    CheckCancel(cancelled);
    if (!summary.appendedBatches) return summary;
    StagedFile target(path);
    std::uint64_t outputBytes = length.QuadPart ? static_cast<std::uint64_t>(length.QuadPart)
        : static_cast<std::uint64_t>(HistoryCsvHeader().size() + 5);
    if (length.QuadPart) {
        LARGE_INTEGER start{}; if (!SetFilePointerEx(source.Get(), start, nullptr, FILE_BEGIN)) IoError("Could not rewind export");
        std::array<char, 65536> buffer{};
        for (;;) { CheckCancel(cancelled); DWORD read = 0; if (!ReadFile(source.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) IoError("Could not copy export"); if (!read) break; Write(target.Get(), std::string(buffer.data(), read), cancelled); }
    } else Write(target.Get(), std::string("\xEF\xBB\xBF") + HistoryCsvHeader() + "\r\n", cancelled);
    for (const auto& entry : entries) if (!complete.count(entry.id))
        for (std::size_t i = 0; i < entry.batch.results.size(); ++i) {
            CheckCancel(cancelled); const auto row = HistoryCsvRow(entry, i);
            if (row.size() > MaxCsvBytes - outputBytes) throw std::runtime_error("Combined export would exceed 512 MiB. Choose a new CSV file or fewer batches.");
            Write(target.Get(), row, cancelled); outputBytes += row.size();
        }
    source.Close(); target.Commit(exists, cancelled); return summary;
}
}
