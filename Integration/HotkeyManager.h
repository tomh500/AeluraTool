#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <optional>
namespace aelura::hotkeys {
struct Binding { std::wstring id, title, category; UINT modifiers{}, key{}; bool enabled{}, registered{}; int slot{}; bool notify{true}; };
inline bool defaultNotify(std::wstring const& id) { return id != L"capture" && id != L"print" && id != L"preview" && id != L"record" && id != L"replay-save"; }
inline bool shouldNotify(Binding const& binding, bool global) { return global && binding.notify; }
struct Manager {
    HWND window{}; std::filesystem::path config; std::vector<Binding> bindings;
    void load(HWND hwnd, std::filesystem::path path);
    bool save(size_t index, UINT modifiers, UINT key, bool enabled, std::wstring& error, std::optional<bool> notify = std::nullopt);
    void release();
    void localize();
};
std::wstring label(Binding const& binding);
}
