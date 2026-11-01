#include "SingleInstance.h"
#include <objbase.h>
#include <stdexcept>
#include <thread>
#include <exception>

namespace {
void CheckInstance(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct InstanceHandle {
    HANDLE value = nullptr;
    ~InstanceHandle() { if (value) CloseHandle(value); }
};
struct InstanceProcess {
    HANDLE value = nullptr;
    ~InstanceProcess() {
        if (!value) return;
        // A failed assertion must not leave our helper waiting in the background.
        if (WaitForSingleObject(value, 0) == WAIT_TIMEOUT) {
            TerminateProcess(value, 99); WaitForSingleObject(value, 10000);
        }
        CloseHandle(value);
    }
    void Start(const std::wstring& arguments) {
        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        CheckInstance(length && length < std::size(executable), "Could not locate test executable.");
        std::wstring command = L"\"" + std::wstring(executable) + L"\" " + arguments;
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        CheckInstance(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != FALSE, "Could not start the isolated instance probe.");
        value = process.hProcess; CloseHandle(process.hThread);
    }
    DWORD Finish() {
        CheckInstance(WaitForSingleObject(value, 10000) == WAIT_OBJECT_0, "Instance probe timed out.");
        DWORD code = 999;
        CheckInstance(GetExitCodeProcess(value, &code) != FALSE, "Could not read instance probe result.");
        return code;
    }
};
DWORD Probe(const std::wstring& name) {
    InstanceProcess process;
    process.Start(L"--instance-probe \"" + name + L"\"");
    return process.Finish();
}
}
int SingleInstanceProbe(const std::wstring& name) {
    try { ui::SingleInstance guard(name); return guard.IsPrimary() ? 0 : 10; }
    catch (...) { return 20; }
}
int SingleInstanceCrashProbe(const std::wstring& name, const std::wstring& readyName, const std::wstring& releaseName) {
    try {
        ui::SingleInstance guard(name);
        if (!guard.IsPrimary()) return 21;
        InstanceHandle ready{OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName.c_str())};
        InstanceHandle release{OpenEventW(SYNCHRONIZE, FALSE, releaseName.c_str())};
        if (!ready.value || !release.value || !SetEvent(ready.value)) return 22;
        if (WaitForSingleObject(release.value, 10000) != WAIT_OBJECT_0) return 23;
        ExitProcess(77); // Deliberately bypass the guard destructor, as on a crash.
    } catch (...) { return 20; }
}
void SingleInstanceTests() {
    GUID id{}; CheckInstance(SUCCEEDED(CoCreateGuid(&id)), "Could not create instance-test ID.");
    wchar_t suffix[40]{}; CheckInstance(StringFromGUID2(id, suffix, 40) != 0, "Could not format instance-test ID.");
    const auto name = std::wstring(L"Local\\DiceRoller.InstanceTest.") + suffix;
    CheckInstance(Probe(name) == 0, "First process must acquire instance ownership.");
    {
        ui::SingleInstance primary(name);
        CheckInstance(primary.IsPrimary(), "Primary instance was rejected.");
        CheckInstance(Probe(name) == 10, "Another process was allowed to own the same instance.");
        { ui::SingleInstance duplicate(name); CheckInstance(!duplicate.IsPrimary(), "Recursive same-thread ownership bypassed the instance guard."); }
        CheckInstance(Probe(name) == 10, "Rejecting one duplicate released the primary lock.");
        bool rejected = false; std::exception_ptr failure;
        std::thread contender([&] {
            try { ui::SingleInstance second(name); rejected = !second.IsPrimary(); }
            catch (...) { failure = std::current_exception(); }
        });
        contender.join(); if (failure) std::rethrow_exception(failure);
        CheckInstance(rejected, "Concurrent initialization bypassed instance ownership.");
    }
    CheckInstance(Probe(name) == 0, "Normal shutdown did not release instance ownership.");
    {
        const auto readyName = name + L".ready", releaseName = name + L".release";
        InstanceHandle ready{CreateEventW(nullptr, TRUE, FALSE, readyName.c_str())};
        InstanceHandle release{CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str())};
        CheckInstance(ready.value && release.value, "Could not create crash-probe events.");
        InstanceProcess process;
        process.Start(L"--instance-crash-probe \"" + name + L"\" \"" + readyName + L"\" \"" + releaseName + L"\"");
        const HANDLE events[] = {ready.value, process.value};
        CheckInstance(WaitForMultipleObjects(2, events, FALSE, 10000) == WAIT_OBJECT_0, "Crash probe did not acquire ownership.");
        ui::SingleInstance retainedHandle(name);
        CheckInstance(!retainedHandle.IsPrimary(), "Crash-probe child was not exclusive.");
        CheckInstance(SetEvent(release.value) != FALSE && process.Finish() == 77, "Crash probe did not exit without cleanup.");
        { ui::SingleInstance recovered(name);
          CheckInstance(recovered.IsPrimary(), "Abandoned instance mutex could not be recovered.");
          CheckInstance(Probe(name) == 10, "Recovered instance does not exclude duplicates."); }
    }
    CheckInstance(Probe(name) == 0, "Crash recovery left an instance locked.");
    {
        InstanceHandle collision{CreateEventW(nullptr, TRUE, FALSE, name.c_str())};
        CheckInstance(collision.value != nullptr && Probe(name) == 20, "An incompatible named object did not fail safely.");
    }
    for (const auto& invalid : {std::wstring{}, std::wstring(L"bad\0name", 8)}) {
        bool rejected = false;
        try { ui::SingleInstance guard(invalid); } catch (const std::invalid_argument&) { rejected = true; }
        CheckInstance(rejected, "An invalid instance-lock name was accepted.");
    }
    const auto production = ui::SingleInstance::UserInstanceName();
    CheckInstance(production.find(L"Global\\DiceRoller.User.S-1-") == 0, "Instance name must protect the same user's data across Windows sessions.");
    CheckInstance(ui::ActivationMessage() != 0, "Instance activation message was unavailable.");
}
