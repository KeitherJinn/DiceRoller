#include "SingleInstance.h"
#include <sddl.h>
#include <stdexcept>
#include <vector>
#include <mutex>
#include <set>

#pragma comment(lib, "Advapi32.lib")
namespace ui {
namespace {
[[noreturn]] void InstanceError(const char* action) {
    const auto error = GetLastError();
    throw std::runtime_error(std::string(action) + " (Windows error " + std::to_string(error) + ").");
}
std::mutex& RegistryMutex() { static std::mutex value; return value; }
std::set<std::wstring>& PrimaryNames() { static std::set<std::wstring> value; return value; }
}
std::wstring SingleInstance::UserInstanceName() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) InstanceError("Could not identify this Windows user");
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{token};
    DWORD size = 0;
    const auto query = GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (query || GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size) InstanceError("Could not read this Windows user");
    std::vector<BYTE> bytes(size);
    if (!GetTokenInformation(token, TokenUser, bytes.data(), size, &size)) InstanceError("Could not read this Windows user");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) InstanceError("Could not identify this Windows user");
    struct Free { LPWSTR value; ~Free() { LocalFree(value); } } free{sid};
    const auto name = std::wstring(L"Global\\DiceRoller.User.") + sid + L".v1";
    return name;
}
SingleInstance::SingleInstance(const std::wstring& name) : name_(name) {
    if (name.empty() || name.find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("The instance lock must have a nonempty name without embedded nulls.");
    // Windows mutexes are recursive for one thread. A second guard in this
    // process must still be secondary rather than recursively claiming ownership.
    std::lock_guard<std::mutex> registry(RegistryMutex());
    if (PrimaryNames().count(name)) return;
    mutex_ = CreateMutexW(nullptr, TRUE, name.c_str());
    if (!mutex_) InstanceError("Could not protect the shared Dice Roller data");
    if (GetLastError() != ERROR_ALREADY_EXISTS) primary_ = true;
    else {
        const auto wait = WaitForSingleObject(mutex_, 0);
        primary_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!primary_ && wait != WAIT_TIMEOUT) {
            const auto error = GetLastError(); CloseHandle(mutex_); mutex_ = nullptr;
            SetLastError(error); InstanceError("Could not check the running Dice Roller instance");
        }
    }
    if (primary_) {
        try { PrimaryNames().insert(name_); }
        catch (...) { ReleaseMutex(mutex_); CloseHandle(mutex_); mutex_ = nullptr; primary_ = false; throw; }
    }
}
SingleInstance::~SingleInstance() {
    if (primary_) {
        std::lock_guard<std::mutex> registry(RegistryMutex());
        ReleaseMutex(mutex_);
        PrimaryNames().erase(name_);
    }
    if (mutex_) CloseHandle(mutex_);
}
UINT ActivationMessage() {
    static const UINT message = RegisterWindowMessageW(L"DiceRoller.ActivateExistingInstance.v1");
    return message;
}
bool ActivateExistingInstance() {
    const auto message = ActivationMessage();
    if (!message) return false;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (const auto window = FindWindowW(L"DiceGUIClass", nullptr)) {
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            AllowSetForegroundWindow(process);
            return PostMessageW(window, message, 0, 0) != FALSE;
        }
        Sleep(50);
    }
    return false;
}
}
