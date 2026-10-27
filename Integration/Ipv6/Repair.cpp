#include "Core.h"
#include "../Privileges.h"
#include <netcfgx.h>
#include <wrl/client.h>
#include <shellapi.h>
#include <stdexcept>
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"uuid.lib")
#pragma comment(lib,"advapi32.lib")
namespace aelura::ipv6 {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t parameters[]=L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters";
constexpr wchar_t recovery[]=L"SOFTWARE\\AeluraTool\\Ipv6Recovery";
void check(HRESULT hr) { if(FAILED(hr))throw std::runtime_error("Network configuration unavailable"); }
bool guid(std::wstring const& id,GUID& value) {return id.size()==38 && id.front()==L'{' && id.back()==L'}' && SUCCEEDED(CLSIDFromString(id.c_str(),&value));}
struct NetConfig {
    HRESULT com{CoInitializeEx(nullptr,COINIT_MULTITHREADED)};
    ComPtr<INetCfg> net;ComPtr<INetCfgLock> lock;bool initialized{},locked{};
    explicit NetConfig(bool write) {
        try {
            check(CoCreateInstance(CLSID_CNetCfg,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&net)));
            if(write) {check(net.As(&lock));LPWSTR owner{};auto hr=lock->AcquireWriteLock(3000,L"AeluraTool",&owner);if(owner)CoTaskMemFree(owner);if(hr!=S_OK)throw std::runtime_error("Network configuration busy");locked=true;}
            check(net->Initialize(nullptr));initialized=true;
        } catch(...) {cleanup();throw;}
    }
    void cleanup() {if(initialized)net->Uninitialize();if(locked)lock->ReleaseWriteLock();net.Reset();lock.Reset();if(SUCCEEDED(com))CoUninitialize();initialized=locked=false;com=E_FAIL;}
    ~NetConfig(){cleanup();}
    ComPtr<INetCfgBindingPath> path(std::wstring const& id) {
        GUID wanted{};if(!guid(id,wanted))return {};
        ComPtr<INetCfgComponent> protocol;check(net->FindComponent(L"ms_tcpip6",&protocol));if(!protocol)return {};
        ComPtr<INetCfgComponentBindings> bindings;check(protocol.As(&bindings));
        ComPtr<IEnumNetCfgBindingPath> paths;check(bindings->EnumBindingPaths(EBP_BELOW,&paths));
        ComPtr<INetCfgBindingPath> p;
        while(paths->Next(1,&p,nullptr)==S_OK) {
            ComPtr<IEnumNetCfgBindingInterface> interfaces;check(p->EnumBindingInterfaces(&interfaces));ComPtr<INetCfgBindingInterface> i;
            while(interfaces->Next(1,&i,nullptr)==S_OK) {ComPtr<INetCfgComponent> lower;check(i->GetLowerComponent(&lower));GUID instance{};if(SUCCEEDED(lower->GetInstanceGuid(&instance)) && IsEqualGUID(instance,wanted))return p;i.Reset();}
            p.Reset();
        }return {};
    }
};
struct Key {HKEY h{};~Key(){if(h)RegCloseKey(h);} };
struct Handle {HANDLE h{};~Handle(){if(h)CloseHandle(h);} };
struct Reply {DWORD pid{},result{static_cast<DWORD>(RepairResult::RollbackFailed)};};
struct Exchange {
    std::wstring base;Handle mapping,started,ack,done;Reply* reply{};
    Exchange(std::wstring const& nonce,bool create):base(L"Local\\AeluraTool.IPv6."+nonce) {
        auto mapName=base+L".Result";
        mapping.h=create?CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Reply),mapName.c_str()):OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,mapName.c_str());
        if(create && mapping.h && GetLastError()==ERROR_ALREADY_EXISTS)throw std::runtime_error("Duplicate exchange");
        if(mapping.h)reply=static_cast<Reply*>(MapViewOfFile(mapping.h,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Reply)));
        auto event=[&](wchar_t const* suffix) {auto name=base+suffix;return create?CreateEventW(nullptr,TRUE,FALSE,name.c_str()):OpenEventW(SYNCHRONIZE|EVENT_MODIFY_STATE,FALSE,name.c_str());};
        started.h=event(L".Started");ack.h=event(L".Ack");done.h=event(L".Done");
        if(!reply || !started.h || !ack.h || !done.h) {if(reply){UnmapViewOfFile(reply);reply=nullptr;}throw std::runtime_error("Exchange unavailable");}
        if(create)*reply=Reply{};
    }
    ~Exchange(){if(reply)UnmapViewOfFile(reply);}
};
DWORD readDword(HKEY h,wchar_t const* name) {DWORD v{},n=sizeof(v);if(RegGetValueW(h,nullptr,name,RRF_RT_REG_DWORD,nullptr,&v,&n)!=ERROR_SUCCESS)throw std::runtime_error("Recovery record incomplete");return v;}
bool put(HKEY h,wchar_t const* name,DWORD v) {return RegSetValueExW(h,name,0,REG_DWORD,reinterpret_cast<BYTE*>(&v),sizeof(v))==ERROR_SUCCESS;}
bool loadJournal(Snapshot& old,Snapshot& next) {
    Key key;if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,recovery,0,KEY_READ|KEY_WOW64_64KEY,&key.h)!=ERROR_SUCCESS)return false;
    wchar_t id[40]{};DWORD size=sizeof(id);
    if(RegGetValueW(key.h,nullptr,L"Adapter",RRF_RT_REG_SZ,nullptr,id,&size)!=ERROR_SUCCESS)throw std::runtime_error("Recovery adapter missing");
    GUID value{};if(!guid(id,value))throw std::runtime_error("Invalid recovery adapter");
    old.adapter=next.adapter=id;old.binding=readDword(key.h,L"OldBinding")!=0;next.binding=readDword(key.h,L"NextBinding")!=0;
    old.config={readDword(key.h,L"OldPresent")!=0,readDword(key.h,L"OldValue")};next.config={readDword(key.h,L"NextPresent")!=0,readDword(key.h,L"NextValue")};return true;
}
struct NativeStore:Store {
    NetConfig net{true};std::wstring id;ComPtr<INetCfgBindingPath> path;bool restart{};
    explicit NativeStore(std::wstring adapter):id(std::move(adapter)){path=net.path(id);if(!path)throw std::runtime_error("Adapter not found");}
    Snapshot read() override {auto state=path->IsEnabled();if(state!=S_OK && state!=S_FALSE)check(state);return {id,state==S_OK,configuration()};}
    bool journal(Snapshot const& old,Snapshot const& next) override {
        Key key;if(RegCreateKeyExW(HKEY_LOCAL_MACHINE,recovery,0,nullptr,0,KEY_WRITE|KEY_WOW64_64KEY,nullptr,&key.h,nullptr)!=ERROR_SUCCESS)return false;
        bool ok=RegSetValueExW(key.h,L"Adapter",0,REG_SZ,reinterpret_cast<BYTE const*>(id.c_str()),static_cast<DWORD>((id.size()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
        ok=put(key.h,L"OldBinding",old.binding)&&ok;ok=put(key.h,L"OldPresent",old.config.present)&&ok;ok=put(key.h,L"OldValue",old.config.value)&&ok;
        ok=put(key.h,L"NextBinding",next.binding)&&ok;ok=put(key.h,L"NextPresent",next.config.present)&&ok;ok=put(key.h,L"NextValue",next.config.value)&&ok;
        ok=ok && RegFlushKey(key.h)==ERROR_SUCCESS;
        if(!ok){RegCloseKey(key.h);key.h=nullptr;RegDeleteKeyExW(HKEY_LOCAL_MACHINE,recovery,KEY_WOW64_64KEY,0);}
        return ok;
    }
    bool binding(bool value) override {auto hr=path->Enable(value?TRUE:FALSE);if(FAILED(hr))return false;restart=restart||hr==NETCFG_S_REBOOT;hr=net.net->Apply();restart=restart||hr==NETCFG_S_REBOOT;return SUCCEEDED(hr);}
    bool config(Config c) override {
        Key key;if(RegOpenKeyExW(HKEY_LOCAL_MACHINE,parameters,0,KEY_SET_VALUE,&key.h)!=ERROR_SUCCESS)return false;
        auto status=c.present?RegSetValueExW(key.h,L"DisabledComponents",0,REG_DWORD,reinterpret_cast<BYTE*>(&c.value),sizeof(c.value)):RegDeleteValueW(key.h,L"DisabledComponents");
        return (status==ERROR_SUCCESS || (!c.present && status==ERROR_FILE_NOT_FOUND)) && RegFlushKey(key.h)==ERROR_SUCCESS;
    }
    bool clear() override {return RegDeleteKeyExW(HKEY_LOCAL_MACHINE,recovery,KEY_WOW64_64KEY,0)==ERROR_SUCCESS;}
};
}
int bindingState(std::wstring const& id) {try {NetConfig net(false);auto path=net.path(id);if(path) {auto state=path->IsEnabled();if(state==S_OK)return 1;if(state==S_FALSE)return 0;}}catch(...){}return -1;}
bool recoveryAvailable() {Key key;return RegOpenKeyExW(HKEY_LOCAL_MACHINE,recovery,0,KEY_READ|KEY_WOW64_64KEY,&key.h)==ERROR_SUCCESS;}
std::wstring recoveryAdapter() {try {Snapshot old,next;if(loadJournal(old,next))return old.adapter;}catch(...){}return {};}
RepairResult repair(std::wstring const& id,bool restore) {
    if(!privileges::elevated())return RepairResult::Denied;
    GUID parsed{};if(!guid(id,parsed))return RepairResult::Failed;
    // NetCfg's machine-wide write lock serializes changes and recovery records.
    try {
        NativeStore store(id);Snapshot old,next;
        if(restore) {if(!loadJournal(old,next))return RepairResult::NoRecord;if(old.adapter!=id)return RepairResult::Conflict;auto result=recover(store,old,next);return result==RepairResult::Success && store.restart?RepairResult::Restart:result;}
        if(recoveryAvailable())return RepairResult::Conflict;
        old=store.read();next=old;next.binding=true;next.config=enabled(old.config);
        if(old.binding && old.config==next.config)return RepairResult::Success;
        auto result=transact(store,old,next,false);
        return result==RepairResult::Success && store.restart?RepairResult::Restart:result;
    } catch(...) {return RepairResult::Failed;}
}
RepairResult launchRepair(HWND owner,std::wstring const& id,bool restore) {
    GUID value{};if(!guid(id,value))return RepairResult::Failed;
    if(privileges::elevated())return repair(id,restore);
    try {
        GUID token{};if(FAILED(CoCreateGuid(&token)))return RepairResult::Failed;wchar_t nonce[40]{};StringFromGUID2(token,nonce,40);Exchange exchange(nonce,true);
        auto exe=privileges::executable();auto target=privileges::activationTarget(privileges::applicationId(),exe);
        auto args=std::wstring(restore?L"--ipv6-restore ":L"--ipv6-enable ")+id+L" "+nonce;
        auto folder=exe.parent_path();SHELLEXECUTEINFOW info{sizeof(info)};info.hwnd=owner;info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI;
        info.lpVerb=L"runas";info.lpFile=target.c_str();info.lpParameters=args.c_str();info.lpDirectory=folder.c_str();info.nShow=SW_HIDE;
        if(!ShellExecuteExW(&info))return GetLastError()==ERROR_CANCELLED?RepairResult::Denied:RepairResult::Failed;
        Handle launcher{info.hProcess};
        // Shell package activation can return a launcher process instead of the helper.
        // A two-phase handshake prevents late activation from changing settings after timeout.
        if(WaitForSingleObject(exchange.started.h,30000)!=WAIT_OBJECT_0)return RepairResult::Failed;
        Handle helper{OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,exchange.reply->pid)};
        if(!helper.h)return RepairResult::Failed;
        wchar_t helperPath[32768]{};DWORD pathSize=32768;
        if(!QueryFullProcessImageNameW(helper.h,0,helperPath,&pathSize) || _wcsicmp(helperPath,exe.c_str()))return RepairResult::Failed;
        SetEvent(exchange.ack.h);HANDLE waiting[]{exchange.done.h,helper.h};auto status=WaitForMultipleObjects(2,waiting,FALSE,INFINITE);
        if(status!=WAIT_OBJECT_0 && WaitForSingleObject(exchange.done.h,0)!=WAIT_OBJECT_0)return RepairResult::RollbackFailed;
        auto result=exchange.reply->result;return result<=static_cast<DWORD>(RepairResult::NoRecord)?static_cast<RepairResult>(result):RepairResult::Failed;
    }catch(...){return RepairResult::Failed;}
}
bool helperLaunch() {
    int count{};auto args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)return false;
    bool helper=count>=2 && (std::wstring(args[1])==L"--ipv6-enable" || std::wstring(args[1])==L"--ipv6-restore");
    if(!helper){LocalFree(args);return false;}
    auto result=RepairResult::Failed;
    GUID adapter{},nonce{};
    if(count==4 && guid(args[2],adapter) && guid(args[3],nonce) && privileges::elevated()) {
        try {
            Exchange exchange(args[3],false);exchange.reply->pid=GetCurrentProcessId();SetEvent(exchange.started.h);
            if(WaitForSingleObject(exchange.ack.h,10000)==WAIT_OBJECT_0)result=repair(args[2],std::wstring(args[1])==L"--ipv6-restore");
            exchange.reply->result=static_cast<DWORD>(result);SetEvent(exchange.done.h);
        }catch(...){}
    }
    LocalFree(args);ExitProcess(static_cast<UINT>(result));
}
}
