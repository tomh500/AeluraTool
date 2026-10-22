#include "I18n.h"
#include <windows.h>
#include <atomic>
#include <array>
#include <unordered_map>
#include <stdexcept>
#include "../Tools/ScreenShotPlus/include/nlohmann/json.hpp"
namespace aelura::i18n {
namespace {
using Dictionary = std::unordered_map<std::string, std::wstring>;
int systemLanguage() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? 0 : 1; }
std::atomic<int> active{systemLanguage()};
Dictionary load(int resource) {
    auto module = GetModuleHandleW(nullptr); auto entry = FindResourceW(module, MAKEINTRESOURCEW(resource), RT_RCDATA);
    if (!entry) throw std::runtime_error("Language resources are missing.");
    auto bytes = static_cast<char const*>(LockResource(LoadResource(module, entry))); auto size = SizeofResource(module, entry);
    auto json = nlohmann::json::parse(bytes, bytes + size); Dictionary result;
    for (auto const& item : json.items()) {
        auto value = item.value().get<std::string>(); int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        std::wstring wide(count, L'\0'); MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), wide.data(), count);
        result.emplace(item.key(), std::move(wide));
    }
    return result;
}
auto const& dictionaries() { static const std::array<Dictionary, 2> data{load(6001), load(6002)}; return data; }
}
int resolve(int preference, std::wstring_view locale) {
    if (preference == 1) return 0; if (preference == 2) return 1;
    return locale.size() >= 2 && (locale[0] == L'z' || locale[0] == L'Z') && (locale[1] == L'h' || locale[1] == L'H') ? 0 : 1;
}
void select(int preference) { active.store(preference == 1 ? 0 : preference == 2 ? 1 : systemLanguage()); }
bool chinese() { return active.load() == 0; }
wchar_t const* tr(std::string_view key) {
    auto const& data = dictionaries(); auto name = std::string(key);
    auto const& selected = data[active.load()]; auto found = selected.find(name);
    if (found != selected.end()) return found->second.c_str();
    found = data[1].find(name); if (found != data[1].end()) return found->second.c_str();
    throw std::runtime_error("Missing translation: " + name);
}
std::string utf8(std::string_view key) {
    auto value = tr(key); int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0'); WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), count, nullptr, nullptr); result.pop_back(); return result;
}
wchar_t const* category(int index) {
    const char* keys[] = {"nav.all", "nav.images", "nav.files", "nav.games"}; return tr(keys[index >= 0 && index < 4 ? index : 0]);
}
}
