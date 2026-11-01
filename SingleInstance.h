#pragma once
#include <windows.h>
#include <string>

namespace ui {
// Held from before configuration/history reads until all application work ends.
// The production name is shared by x86/x64 and Debug/Release, across sessions
// belonging to the same Windows user. Test names must use a separate suffix.
// Mutex ownership is thread-affine: construct and destroy on the same thread.
class SingleInstance {
public:
    explicit SingleInstance(const std::wstring& name = UserInstanceName());
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    bool IsPrimary() const { return primary_; }
    static std::wstring UserInstanceName();
private:
    HANDLE mutex_ = nullptr;
    bool primary_ = false;
    std::wstring name_;
};
UINT ActivationMessage();
bool ActivateExistingInstance();
}
