#include "../Integration/Privileges.h"
#include <taskschd.h>
#include <comdef.h>
#include <wrl/client.h>
#include <iostream>
#include <stdexcept>
#include <sddl.h>
#include <vector>
namespace {
void check(bool value, char const* message) { if (!value) throw std::runtime_error(message); }
bool currentUser(std::wstring const& identity) {
    PSID parsed{};
    if (ConvertStringSidToSidW(identity.c_str(), &parsed)) {
        PWSTR canonical{}; ConvertSidToStringSidW(parsed, &canonical);
        bool same = canonical && aelura::privileges::userSid() == canonical;
        if (canonical) LocalFree(canonical); LocalFree(parsed); return same;
    }
    DWORD bytes{}, chars{}; SID_NAME_USE use{}; LookupAccountNameW(nullptr, identity.c_str(), nullptr, &bytes, nullptr, &chars, &use);
    std::vector<char> data(bytes); std::vector<wchar_t> domain(chars);
    if (!bytes || !LookupAccountNameW(nullptr, identity.c_str(), data.data(), &bytes, domain.data(), &chars, &use)) return false;
    PWSTR canonical{}; if (!ConvertSidToStringSidW(data.data(), &canonical)) return false;
    bool same = aelura::privileges::userSid() == canonical; LocalFree(canonical); return same;
}
}
void run_privileges_tests(std::filesystem::path const& root) {
    using namespace aelura::privileges;
    using Microsoft::WRL::ComPtr;
    auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    check(SUCCEEDED(initialized), "privilege test COM initialization failed");
    try {
        check(!userSid().empty() && executable().is_absolute(), "process identity unavailable");
        check(activationTarget(L"Sample_family!App",executable())==L"shell:AppsFolder\\Sample_family!App","packaged elevation must use shell activation");
        check(activationTarget({},executable())==executable().wstring(),"unpackaged test host must retain its executable target");
        // Build the real task definition through Windows COM, without installing a task.
        ComPtr<ITaskDefinition> definition; check(SUCCEEDED(startupDefinition(&definition)), "startup definition creation failed");
        ComPtr<IPrincipal> principal; check(SUCCEEDED(definition->get_Principal(&principal)), "missing startup principal");
        TASK_RUNLEVEL_TYPE level{}; TASK_LOGON_TYPE type{}; BSTR text{};
        check(SUCCEEDED(principal->get_RunLevel(&level)) && level == TASK_RUNLEVEL_HIGHEST, "startup run level is not highest");
        check(SUCCEEDED(principal->get_LogonType(&type)) && type == TASK_LOGON_INTERACTIVE_TOKEN, "startup must require interactive sign-in");
        check(SUCCEEDED(principal->get_UserId(&text)), "startup user missing");
        auto user = std::wstring(text); SysFreeString(text); text = nullptr; check(currentUser(user), "startup user is not current user");
        ComPtr<ITriggerCollection> triggers; definition->get_Triggers(&triggers); LONG count{}; triggers->get_Count(&count);
        check(count == 1, "unexpected startup triggers"); ComPtr<ITrigger> trigger; triggers->get_Item(1, &trigger);
        ComPtr<ILogonTrigger> logon; check(SUCCEEDED(trigger.As(&logon)), "startup is not a logon trigger");
        logon->get_UserId(&text); user = text; SysFreeString(text); text = nullptr; check(currentUser(user), "logon trigger applies to another user");
        ComPtr<IActionCollection> actions; definition->get_Actions(&actions); actions->get_Count(&count); check(count == 1, "unexpected startup actions");
        ComPtr<IAction> action; actions->get_Item(1, &action); ComPtr<IExecAction> exec; check(SUCCEEDED(action.As(&exec)), "startup action is not executable");
        exec->get_Path(&text); auto path = std::filesystem::path(text); SysFreeString(text); text = nullptr;
        check(path == executable(), "startup does not point to deployed executable");
        exec->get_Arguments(&text); auto arguments = std::wstring(text); SysFreeString(text);
        check(arguments == L"--startup", "startup does not activate tray mode");
        check(!requestAutomatic(nullptr, root / L"absent.ini"), "missing preference unexpectedly requested elevation");
        // A forged helper request must fail without installing or disabling any task.
        auto command = L"\"" + executable().wstring() + L"\" --configure-admin-startup 1 S-1-0-0";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
        check(CreateProcessW(executable().c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process), "helper rejection test could not launch");
        auto waited = WaitForSingleObject(process.hProcess, 10000); DWORD code{}; GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        check(waited == WAIT_OBJECT_0 && code == ERROR_ACCESS_DENIED, "helper accepted an unrelated user");
    } catch (...) { CoUninitialize(); throw; }
    CoUninitialize();
    std::cout << "PASS: process identity, interactive highest-privilege startup definition, tray arguments, helper identity rejection (no task registered)\n";
}
