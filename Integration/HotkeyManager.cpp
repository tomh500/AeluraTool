#include "I18n.h"
#include "HotkeyManager.h"
namespace aelura::hotkeys {
using aelura::i18n::tr;
namespace {
bool valid(UINT mods, UINT key, bool print) {
    return print ? mods == 0 && key == VK_SNAPSHOT : mods && !(mods & ~(MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) &&
        ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || (key >= VK_F1 && key <= VK_F24));
}
}
std::wstring label(Binding const& b) {
    std::wstring s;
    if (b.modifiers & MOD_CONTROL) s += L"Ctrl + "; if (b.modifiers & MOD_ALT) s += L"Alt + ";
    if (b.modifiers & MOD_SHIFT) s += L"Shift + "; if (b.modifiers & MOD_WIN) s += L"Win + ";
    s += b.key == VK_SNAPSHOT ? L"PrintScreen" : b.key >= VK_F1 && b.key <= VK_F24 ? L"F" + std::to_wstring(b.key - VK_F1 + 1) : std::wstring(1, static_cast<wchar_t>(b.key));
    return s;
}
void Manager::load(HWND hwnd, std::filesystem::path path) {
    window = hwnd; config = std::move(path);
    bindings = {{L"capture", tr("capture.start"), tr("nav.images"), MOD_CONTROL | MOD_ALT, 'A', true},
        {L"preview", tr("capture.recent"), tr("nav.images"), MOD_CONTROL | MOD_ALT, 'P', false},
        {L"move", tr("keys.move"), tr("nav.files"), MOD_CONTROL | MOD_ALT, 'M', false},
        {L"strike", tr("keys.strike"), tr("nav.games"), MOD_CONTROL | MOD_ALT, 'S', false},
        {L"print", tr("keys.print"), tr("nav.images"), 0, VK_SNAPSHOT, false}};
    for (size_t i = 0; i < bindings.size(); ++i) {
        auto& b = bindings[i]; b.slot = 200 + static_cast<int>(i) * 2;
        const auto section = L"Hotkey." + b.id;
        auto read = [&](wchar_t const* key, UINT fallback) { return GetPrivateProfileIntW(section.c_str(), key, fallback, config.c_str()); };
        b.modifiers = read(L"Modifiers", b.modifiers); b.key = read(L"Key", b.key); b.enabled = read(L"Enabled", b.enabled) != 0;
        // Upgrade the previous capture binding without losing the user's choice.
        if (b.id == L"capture") {
            b.modifiers = read(L"Modifiers", GetPrivateProfileIntW(L"Capture", L"Modifiers", b.modifiers, config.c_str()));
            b.key = read(L"Key", GetPrivateProfileIntW(L"Capture", L"Key", b.key, config.c_str()));
            b.enabled = read(L"Enabled", GetPrivateProfileIntW(L"Capture", L"Enabled", b.enabled, config.c_str())) != 0;
        }
        if (b.id == L"print") b.enabled = read(L"Enabled", GetPrivateProfileIntW(L"Capture", L"Enabled", 1, config.c_str()) && GetPrivateProfileIntW(L"Capture", L"PrintScreen", 0, config.c_str())) != 0;
        if (valid(b.modifiers, b.key, b.id == L"print") && b.enabled) b.registered = RegisterHotKey(window, b.slot, b.modifiers | MOD_NOREPEAT, b.key) != 0;
    }
}
bool Manager::save(size_t index, UINT mods, UINT key, bool enabled, std::wstring& error) {
    auto& b = bindings.at(index);
    if (!valid(mods, key, b.id == L"print")) { error = tr("error.invalid_shortcut"); return false; }
    const bool same = b.registered && mods == b.modifiers && key == b.key;
    const int candidate = b.slot % 2 ? b.slot - 1 : b.slot + 1;
    if (enabled && !same && !RegisterHotKey(window, candidate, mods | MOD_NOREPEAT, key)) { error = tr("error.shortcut_busy"); return false; }
    std::error_code ec; std::filesystem::create_directories(config.parent_path(), ec);
    auto temp = config; temp += L".tmp";
    bool ok = !ec;
    if (ok && std::filesystem::exists(config, ec)) ok = CopyFileW(config.c_str(), temp.c_str(), FALSE) != 0;
    else if (ok) { DeleteFileW(temp.c_str()); }
    const auto section = L"Hotkey." + b.id;
    if (ok) ok = WritePrivateProfileStringW(section.c_str(), L"Modifiers", std::to_wstring(mods).c_str(), temp.c_str()) &&
        WritePrivateProfileStringW(section.c_str(), L"Key", std::to_wstring(key).c_str(), temp.c_str()) &&
        WritePrivateProfileStringW(section.c_str(), L"Enabled", enabled ? L"1" : L"0", temp.c_str());
    if (ok) {
        // The profile cache flush returns zero even on success; verify the persisted file separately.
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temp.c_str());
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ok = file != INVALID_HANDLE_VALUE;
        if (ok) { ok = FlushFileBuffers(file) != 0; CloseHandle(file); }
    }
    if (ok) ok = MoveFileExW(temp.c_str(), config.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) { DeleteFileW(temp.c_str()); if (enabled && !same) UnregisterHotKey(window, candidate); error = tr("error.preference_save"); return false; }
    if (b.registered && (!enabled || !same)) UnregisterHotKey(window, b.slot);
    if (enabled && !same) b.slot = candidate;
    b.modifiers = mods; b.key = key; b.enabled = enabled; b.registered = enabled; return true;
}
void Manager::release() { for (auto& b : bindings) { if (b.registered) UnregisterHotKey(window, b.slot); b.registered = false; } }
void Manager::localize() {
    const char* titles[] = {"capture.start", "capture.recent", "keys.move", "keys.strike", "keys.print"};
    for (size_t i = 0; i < bindings.size(); ++i) { bindings[i].title = tr(titles[i]); bindings[i].category = i18n::category(i == 2 ? 2 : i == 3 ? 3 : 1); }
}
}
