#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
namespace aelura::hotkeys {
struct Binding { std::wstring id, title, category; UINT modifiers{}, key{}; bool enabled{}, registered{}; int slot{}; };
struct Manager {
    HWND window{}; std::filesystem::path config; std::vector<Binding> bindings;
    void load(HWND hwnd, std::filesystem::path path);
    bool save(size_t index, UINT modifiers, UINT key, bool enabled, std::wstring& error);
    void release();
    void localize();
};
std::wstring label(Binding const& binding);
}
