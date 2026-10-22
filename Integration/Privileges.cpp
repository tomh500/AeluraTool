#include "Privileges.h"
#include <shellapi.h>
#include <sddl.h>
#include <taskschd.h>
#include <comdef.h>
#include <wrl/client.h>
#include <vector>
#include <appmodel.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsuppw.lib")
namespace aelura::privileges {
namespace {
using Microsoft::WRL::ComPtr;
struct Handle { HANDLE value{}; ~Handle() { if (value) CloseHandle(value); } };
void check(HRESULT hr) { if (FAILED(hr)) throw hr; }
std::wstring sid(HANDLE process) {
    Handle token; if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return {};
    DWORD bytes{}; GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    std::vector<char> data(bytes); if (!bytes || !GetTokenInformation(token.value, TokenUser, data.data(), bytes, &bytes)) return {};
    PWSTR text{}; if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &text)) return {};
    std::wstring result(text); LocalFree(text); return result;
}
std::wstring taskName() { return L"AeluraTool.Startup." + userSid(); }
ComPtr<ITaskService> service() {
    ComPtr<ITaskService> value; check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&value)));
    check(value->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t())); return value;
}
Launch launch(HWND owner, std::wstring const& arguments) {
    Launch result; auto path = executable();
    if (path.empty()) { result.error = ERROR_FILE_NOT_FOUND; return result; }
    SHELLEXECUTEINFOW info{sizeof(info)}; info.hwnd = owner;
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    const auto target = activationTarget(applicationId(), path);
    info.lpVerb = L"runas"; info.lpFile = target.c_str(); info.lpParameters = arguments.c_str();
    auto folder = path.parent_path(); info.lpDirectory = folder.c_str(); info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) { result.started = true; result.process = info.hProcess; }
    else result.error = GetLastError();
    return result;
}
}
bool elevated() {
    Handle token; TOKEN_ELEVATION info{}; DWORD bytes{};
    return OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) &&
        GetTokenInformation(token.value, TokenElevation, &info, sizeof(info), &bytes) && info.TokenIsElevated;
}
std::wstring applicationId() {
    UINT32 length{};
    if(GetCurrentApplicationUserModelId(&length,nullptr)!=ERROR_INSUFFICIENT_BUFFER)return {};
    std::wstring value(length,L'\0');
    if(GetCurrentApplicationUserModelId(&length,value.data())!=ERROR_SUCCESS || length<2)return {};
    value.resize(length-1);return value;
}
std::wstring activationTarget(std::wstring const& appId, std::filesystem::path const& fallback) {
    return appId.empty()?fallback.wstring():L"shell:AppsFolder\\"+appId;
}
std::wstring userSid() { return sid(GetCurrentProcess()); }
std::filesystem::path executable() {
    wchar_t path[32768]{}; auto size = GetModuleFileNameW(nullptr, path, 32768);
    return size && size < 32768 ? std::filesystem::path(path) : std::filesystem::path{};
}
Launch restart(HWND owner) {
    GUID id{}; if (FAILED(CoCreateGuid(&id))) return {{}, {}, ERROR_GEN_FAILURE};
    wchar_t guid[40]{}; StringFromGUID2(id, guid, 40);
    auto name = std::wstring(L"Local\\AeluraTool.Elevate.") + guid;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    if (!ready) return {{}, {}, GetLastError()};
    auto result = launch(owner, L"--elevated-restart " + std::to_wstring(GetCurrentProcessId()) + L" " + name);
    if (!result.started) CloseHandle(ready); else result.ready = ready;
    return result;
}
bool startupEnabled() {
    try {
        auto connection = service(); ComPtr<ITaskFolder> folder; check(connection->GetFolder(_bstr_t(L"\\"), &folder));
        ComPtr<IRegisteredTask> task; if (FAILED(folder->GetTask(_bstr_t(taskName().c_str()), &task))) return false;
        VARIANT_BOOL enabled{}; check(task->get_Enabled(&enabled)); return enabled == VARIANT_TRUE;
    } catch (...) { return false; }
}
HRESULT startupDefinition(ITaskDefinition** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    try {
        auto connection = service();
        ComPtr<ITaskDefinition> definition; check(connection->NewTask(0, &definition));
        const auto user = userSid(); const auto path = executable(); if (path.empty() || user.empty()) return E_FAIL;
        ComPtr<IPrincipal> principal; check(definition->get_Principal(&principal));
        check(principal->put_UserId(_bstr_t(user.c_str())));
        check(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)); check(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST));
        ComPtr<ITaskSettings> settings; check(definition->get_Settings(&settings));
        check(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)); check(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));
        check(settings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"))); check(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));
        check(settings->put_StartWhenAvailable(VARIANT_TRUE));
        ComPtr<ITriggerCollection> triggers; check(definition->get_Triggers(&triggers));
        ComPtr<ITrigger> trigger; check(triggers->Create(TASK_TRIGGER_LOGON, &trigger));
        ComPtr<ILogonTrigger> logon; check(trigger.As(&logon)); check(logon->put_UserId(_bstr_t(user.c_str())));
        ComPtr<IActionCollection> actions; check(definition->get_Actions(&actions));
        ComPtr<IAction> action; check(actions->Create(TASK_ACTION_EXEC, &action));
        ComPtr<IExecAction> exec; check(action.As(&exec)); check(exec->put_Path(_bstr_t(path.c_str())));
        check(exec->put_Arguments(_bstr_t(L"--startup"))); check(exec->put_WorkingDirectory(_bstr_t(path.parent_path().c_str())));
        const auto appId=applicationId();
        if(!appId.empty()) {
            wchar_t system[32768]{};if(!GetSystemDirectoryW(system,32768))return HRESULT_FROM_WIN32(GetLastError());
            const auto powershell=std::filesystem::path(system)/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
            auto target=activationTarget(appId,path);std::wstring escaped;for(auto c:target){escaped+=c;if(c==L'\'')escaped+=c;}
            const auto arguments=L"-NoProfile -NonInteractive -WindowStyle Hidden -Command \"Start-Process -Verb RunAs -FilePath '"+escaped+L"' -ArgumentList '--startup'\"";
            check(exec->put_Path(_bstr_t(powershell.c_str())));check(exec->put_Arguments(_bstr_t(arguments.c_str())));
        }
        *result = definition.Detach(); return S_OK;
    } catch (HRESULT hr) { return hr; } catch (...) { return E_FAIL; }
}
bool setStartup(bool enabled) {
    if (!elevated() || userSid().empty()) return false;
    try {
        auto connection = service(); ComPtr<ITaskFolder> folder; check(connection->GetFolder(_bstr_t(L"\\"), &folder));
        if (!enabled) {
            ComPtr<IRegisteredTask> task;
            auto hr = folder->GetTask(_bstr_t(taskName().c_str()), &task);
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return true;
            check(hr); check(task->put_Enabled(VARIANT_FALSE)); return true;
        }
        ComPtr<ITaskDefinition> definition; check(startupDefinition(&definition));
        const auto user = userSid();
        ComPtr<IRegisteredTask> registered;
        check(folder->RegisterTaskDefinition(_bstr_t(taskName().c_str()), definition.Get(), TASK_CREATE_OR_UPDATE,
            _variant_t(user.c_str()), _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(), &registered));
        return true;
    } catch (...) { return false; }
}
Launch configureStartup(HWND owner, bool enabled) {
    GUID id{};if(FAILED(CoCreateGuid(&id)))return {{},{},ERROR_GEN_FAILURE};
    wchar_t guid[40]{};StringFromGUID2(id,guid,40);
    auto name=std::wstring(L"Local\\AeluraTool.Configure.")+guid;
    auto ready=CreateEventW(nullptr,TRUE,FALSE,name.c_str());if(!ready)return {{},{},GetLastError()};
    auto result=launch(owner,std::wstring(L"--configure-admin-startup ")+(enabled?L"1 ":L"0 ")+userSid()+L" "+name);
    if(result.started)result.ready=ready;else CloseHandle(ready);
    return result;
}
bool prepareLaunch(bool& startup) {
    int count{}; auto args = CommandLineToArgvW(GetCommandLineW(), &count); if (!args) return true;
    std::vector<std::wstring> values(args, args + count); LocalFree(args);
    if (count == 2 && (values[1] == L"--startup" || values[1] == L"--startup-auto-elevated")) startup = true;
    if ((count == 4 || count == 5) && values[1] == L"--configure-admin-startup") {
        const bool valid = elevated() && !userSid().empty() && values[3] == userSid() && (values[2] == L"0" || values[2] == L"1");
        const bool success=valid && setStartup(values[2] == L"1");
        if(success && count==5 && values[4].rfind(L"Local\\AeluraTool.Configure.",0)==0) {
            Handle event;event.value=OpenEventW(EVENT_MODIFY_STATE,FALSE,values[4].c_str());if(event.value)SetEvent(event.value);
        }
        ExitProcess(success?0:ERROR_ACCESS_DENIED);
    }
    if (count == 4 && values[1] == L"--elevated-restart") {
        wchar_t* end{}; auto pid = wcstoul(values[2].c_str(), &end, 10);
        if (!elevated() || !pid || !end || *end || values[3].rfind(L"Local\\AeluraTool.Elevate.", 0) != 0) return false;
        Handle parent; parent.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        wchar_t path[32768]{}; DWORD size = 32768;
        if (!parent.value || sid(parent.value) != userSid() || !QueryFullProcessImageNameW(parent.value, 0, path, &size) ||
            _wcsicmp(path, executable().c_str())) return false;
        Handle event; event.value = OpenEventW(EVENT_MODIFY_STATE, FALSE, values[3].c_str());
        if (!event.value || !SetEvent(event.value)) return false;
        if (WaitForSingleObject(parent.value, 30000) != WAIT_OBJECT_0) return false;
    }
    return true;
}
bool requestAutomatic(HWND owner, std::filesystem::path const& config, bool startup) {
    if (elevated() || !GetPrivateProfileIntW(L"Behavior", L"AutoElevate", 0, config.c_str())) return false;
    HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\AeluraTool.Singleton");
    const bool running = existing || GetLastError() == ERROR_ACCESS_DENIED;
    if (existing) CloseHandle(existing);
    if (running) return false;
    auto result = launch(owner, startup ? L"--startup-auto-elevated" : L"--auto-elevated");
    if (result.started) { if(result.process)CloseHandle(result.process); return true; }
    return false; // Including ERROR_CANCELLED: the ordinary window remains usable.
}
}
