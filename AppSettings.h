#pragma once

#include "DiceCore.h"
#include <filesystem>

namespace dice {
constexpr std::size_t MaxPresets = 50;
constexpr std::size_t MaxPresetName = 64;

struct Preset {
    std::wstring name;
    std::wstring notation;
    int times = 1;
    Mode mode = Mode::Normal;
};

// Position is in screen pixels. Size is in device-independent pixels (96 DPI).
// The window layer must fit a restored placement to the current monitor work area.
struct WindowSettings {
    bool valid = false;
    int x = 0;
    int y = 0;
    int width = 840;
    int height = 780;
    bool maximized = false;
};

struct AppSettings {
    std::wstring name = L"roll";
    std::wstring notation = L"1d100+0";
    std::wstring csvPath;
    int times = 1;
    Mode mode = Mode::Normal;
    bool autoSave = true;
    bool showTimestamp = false;
    WindowSettings window;
    std::vector<Preset> presets;
};

struct SettingsLoad {
    AppSettings settings;
    std::wstring warning;
    bool found = false;
};

AppSettings DefaultSettings();
std::filesystem::path DefaultSettingsPath();
void ValidatePreset(const Preset& preset);
void ValidateSettings(const AppSettings& settings);
// Missing settings are normal. Unreadable, unsupported or invalid files return
// the supplied defaults and a warning; no partial configuration is applied.
SettingsLoad LoadSettings(const std::filesystem::path& path, const AppSettings& defaults);
// Preserve an unreadable/unsupported configuration before replacing it with defaults.
// Returns a unique sibling backup path, or an empty path when the source is absent.
// Never overwrites an existing backup; a failed copy throws and leaves the source intact.
std::filesystem::path BackupSettings(const std::filesystem::path& path);
// Serializes only preferences and presets, never generated dice results.
// Validates and flushes a sibling temporary file before atomically replacing path.
// Throws on failure, preserving the existing file until the replacement succeeds.
void SaveSettings(const std::filesystem::path& path, const AppSettings& settings);
}
