#pragma once
#include "DiceCore.h"
#include <deque>
#include <filesystem>

namespace dice {
std::string Utf8(const std::wstring& text);
std::wstring Wide(const std::string& text);
std::string CsvField(const std::string& value);
std::string CsvRow(const Request& request, const Result& result);
const std::string& CsvHeader();
std::string Timestamp();
std::wstring FormatResult(const Request& request, const Result& result, std::size_t index);
std::filesystem::path DefaultDirectory();
std::filesystem::path ResolvePath(const std::wstring& input, const std::filesystem::path& base);
// Stages a complete replacement beside the destination and commits only after flushing.
// Failure/cancellation before commit preserves the original file and removes the staging file.
void AppendCsv(const std::filesystem::path& path, const Batch& batch,
               const CancelCheck& cancelled = {});

class LogBuffer {
public:
    static constexpr std::size_t MaxEntries = 500;
    static constexpr std::size_t MaxCharacters = 120000;
    void Add(std::wstring entry);
    void Clear();
    std::wstring Text() const;
    std::size_t Dropped() const { return dropped_; }
private:
    std::deque<std::wstring> entries_;
    std::size_t characters_ = 0;
    std::size_t dropped_ = 0;
};
}
