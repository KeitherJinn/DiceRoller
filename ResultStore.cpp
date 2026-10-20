#include "ResultStore.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <ctime>
#include <limits>
#include <sstream>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace dice {
namespace {
[[noreturn]] void FileError(const char* action) {
    const auto code = GetLastError();
    throw std::runtime_error(std::string(action) + " (Windows error " + std::to_string(code) + ").");
}
class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value_; }
    void Close() {
        const auto value = value_;
        value_ = INVALID_HANDLE_VALUE;
        if (value != INVALID_HANDLE_VALUE && !CloseHandle(value)) FileError("Could not close CSV");
    }
private:
    HANDLE value_;
};
void CheckCancel(const CancelCheck& cancelled) {
    if (cancelled && cancelled()) throw Cancelled();
}
void Write(HANDLE file, const char* data, std::size_t length) {
    while (length) {
        const DWORD amount = static_cast<DWORD>((std::min)(length, std::size_t(65536)));
        DWORD written = 0;
        if (!WriteFile(file, data, amount, &written, nullptr)) FileError("Could not write CSV");
        if (!written) throw std::runtime_error("CSV write made no progress.");
        data += written;
        length -= written;
    }
}
std::string DiceList(const Group& group, std::size_t limit) {
    std::ostringstream out;
    for (std::size_t i = 0; i < group.dice.size() && i < limit; ++i) {
        if (i) out << ',';
        out << group.dice[i];
    }
    if (group.dice.size() > limit) out << ",... +" << group.dice.size() - limit << " more";
    return out.str();
}
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    if (text.size() > INT_MAX) throw std::length_error("Text is too long.");
    const int count = static_cast<int>(text.size());
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count,
                                          nullptr, 0, nullptr, nullptr);
    if (!length) throw std::runtime_error("Text contains invalid Unicode characters.");
    std::string result(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), count,
                             result.data(), length, nullptr, nullptr)) FileError("UTF-8 conversion failed");
    return result;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    if (text.size() > INT_MAX) throw std::length_error("Text is too long.");
    const int count = static_cast<int>(text.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), count, nullptr, 0);
    if (!length) throw std::runtime_error("Text is not valid UTF-8.");
    std::wstring result(length, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), count,
                            result.data(), length)) FileError("Unicode conversion failed");
    return result;
}

std::string CsvField(const std::string& value) {
    std::string out = "\"";
    for (char ch : value) {
        if (ch == '"') out += '"';
        out += ch;
    }
    return out + '"';
}

const std::string& CsvHeader() {
    static const std::string header = "Name,DiceNotation,Mode,NumDice,Sides,Roll1,Roll2,ChosenRoll,Modifier,FinalResult,Timestamp,Roll1Dice,Roll2Dice";
    return header;
}

std::string CsvRow(const Request& request, const Result& result) {
    std::ostringstream out;
    out << CsvField(Utf8(request.name)) << ',' << CsvField(Utf8(request.notation)) << ','
        << CsvField(ModeName(request.mode)) << ',' << request.spec.count << ',' << request.spec.sides << ','
        << result.first.total << ',';
    if (result.second) out << result.second->total;
    out << ',' << result.chosen << ',' << request.spec.modifier << ',' << result.finalValue << ','
        << CsvField(result.timestamp) << ',' << CsvField(DiceList(result.first, MaxDice)) << ',';
    if (result.second) out << CsvField(DiceList(*result.second, MaxDice));
    out << "\r\n";
    return out.str();
}

std::string Timestamp() {
    const auto now = std::time(nullptr);
    std::tm local{};
    if (localtime_s(&local, &now)) throw std::runtime_error("Could not read local time.");
    char buffer[32]{};
    if (!std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local))
        throw std::runtime_error("Could not format local time.");
    return buffer;
}

std::wstring FormatResult(const Request& request, const Result& result, std::size_t index) {
    std::wostringstream out;
    out << L"Roll " << index << L" for " << request.name << L" [" << request.notation << L"] ";
    out << L'(' << Wide(DiceList(result.first, 30)) << L") = " << result.first.total;
    if (result.second) {
        out << L"; (" << Wide(DiceList(*result.second, 30)) << L") = " << result.second->total
            << L" -> chosen " << result.chosen << L" (" << Wide(ModeName(request.mode)) << L')';
    }
    if (request.spec.modifier != 0)
        out << L" (modifier " << (request.spec.modifier > 0 ? L"+" : L"") << request.spec.modifier << L')';
    out << L" -> Final: " << result.finalValue;
    if (request.showTimestamp) out << L" @ " << Wide(result.timestamp);
    return out.str() + L"\r\n";
}

std::filesystem::path DefaultDirectory() {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
        throw std::runtime_error("Could not locate the local application data folder.");
    const std::filesystem::path root(folder);
    CoTaskMemFree(folder);
    return root / L"DiceRoller" / L"Results";
}

std::filesystem::path ResolvePath(const std::wstring& input, const std::filesystem::path& base) {
    const auto text = Trim(input);
    if (text.empty() || text.find(L'\0') != std::wstring::npos || text.find_first_of(L"\r\n") != std::wstring::npos)
        throw std::invalid_argument("Enter a CSV file name or full path.");
    std::filesystem::path path(text);
    if (path.filename().empty() || path.filename() == L"." || path.filename() == L"..")
        throw std::invalid_argument("The path must include a file name.");
    if ((path.has_root_name() || path.has_root_directory()) && !path.is_absolute())
        throw std::invalid_argument("Use a full absolute path or a path relative to the default results folder.");
    if (!path.has_extension()) path += L".csv";
    if (_wcsicmp(path.extension().c_str(), L".csv") != 0)
        throw std::invalid_argument("Choose a file with the .csv extension.");
    if (!base.is_absolute()) throw std::invalid_argument("The default directory must be absolute.");
    return (path.is_absolute() ? path : base / path).lexically_normal();
}

void AppendCsv(const std::filesystem::path& path, const Batch& batch, const CancelCheck& cancelled) {
    if (batch.results.empty()) throw std::invalid_argument("There are no results to save.");
    CheckCancel(cancelled);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) throw std::runtime_error("Could not create the output directory (error " + std::to_string(ec.value()) + ").");
    auto lockPath = path;
    lockPath += L".lock";
    Handle lock(CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    if (lock.Get() == INVALID_HANDLE_VALUE) FileError("Output is locked, or its .lock companion already exists");

    Handle source(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    const bool exists = source.Get() != INVALID_HANDLE_VALUE;
    if (!exists && GetLastError() != ERROR_FILE_NOT_FOUND) FileError("Could not read the existing CSV");
    LARGE_INTEGER length{};
    if (exists && !GetFileSizeEx(source.Get(), &length)) FileError("Could not read the CSV size");
    if (length.QuadPart > 0) {
        std::string header;
        char ch = 0;
        DWORD read = 0;
        while (header.size() <= CsvHeader().size() + 4) {
            if (!ReadFile(source.Get(), &ch, 1, &read, nullptr)) FileError("Could not read the CSV header");
            if (!read || ch == '\n') break;
            header += ch;
        }
        if (!header.empty() && header.back() == '\r') header.pop_back();
        if (header.compare(0, 3, "\xEF\xBB\xBF") == 0) header.erase(0, 3);
        if (header != CsvHeader())
            throw std::runtime_error("This CSV uses an older or different format. Choose a new CSV file; the existing file was not changed.");
        LARGE_INTEGER start{};
        if (!SetFilePointerEx(source.Get(), start, nullptr, FILE_BEGIN)) FileError("Could not seek in CSV");
    }

    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Could not create a temporary file name.");
    wchar_t suffix[40]{};
    StringFromGUID2(id, suffix, 40);
    auto temp = path;
    temp += std::wstring(L".") + suffix + L".tmp";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{temp};
    Handle target(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (target.Get() == INVALID_HANDLE_VALUE) FileError("Could not create the temporary CSV");
    if (length.QuadPart > 0) {
        std::array<char, 65536> buffer{};
        char last = 0;
        for (;;) {
            CheckCancel(cancelled);
            DWORD read = 0;
            if (!ReadFile(source.Get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                FileError("Could not copy the existing CSV");
            if (!read) break;
            Write(target.Get(), buffer.data(), read);
            last = buffer[read - 1];
        }
        if (last == '\r') Write(target.Get(), "\n", 1);
        else if (last != '\n') Write(target.Get(), "\r\n", 2);
    } else {
        const auto header = std::string("\xEF\xBB\xBF") + CsvHeader() + "\r\n";
        Write(target.Get(), header.data(), header.size());
    }
    for (const auto& result : batch.results) {
        CheckCancel(cancelled);
        const auto row = CsvRow(batch.request, result);
        Write(target.Get(), row.data(), row.size());
    }
    CheckCancel(cancelled);
    if (!FlushFileBuffers(target.Get())) FileError("Could not flush the CSV to disk");
    target.Close();
    // Windows replacement requires the destination's read handle to be closed.
    // The sidecar lock still serializes Dice Roller writers through the commit.
    source.Close();
    CheckCancel(cancelled);
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (exists ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!MoveFileExW(temp.c_str(), path.c_str(), flags)) FileError("Could not commit the CSV; original file preserved");
}

void LogBuffer::Add(std::wstring entry) {
    if (entry.size() > MaxCharacters) {
        entry.resize(MaxCharacters - 32);
        // Never leave half of a UTF-16 surrogate pair at the boundary.
        if (!entry.empty() && entry.back() >= 0xD800 && entry.back() <= 0xDBFF) entry.pop_back();
        entry += L"... [entry shortened]\r\n";
    }
    characters_ += entry.size();
    entries_.push_back(std::move(entry));
    while (entries_.size() > MaxEntries || characters_ > MaxCharacters) {
        characters_ -= entries_.front().size();
        entries_.pop_front();
        ++dropped_;
    }
}
void LogBuffer::Clear() { entries_.clear(); characters_ = 0; dropped_ = 0; }
std::wstring LogBuffer::Text() const {
    std::wstring text;
    text.reserve(characters_);
    for (const auto& entry : entries_) text += entry;
    return text;
}
}
