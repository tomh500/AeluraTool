#pragma once
#include <string>
#include <string_view>
namespace aelura::i18n {
// Stored preference: 0 = system display language, 1 = Chinese, 2 = English.
int resolve(int preference, std::wstring_view systemLanguage);
void select(int preference);
bool chinese();
wchar_t const* tr(std::string_view key);
std::string utf8(std::string_view key);
wchar_t const* category(int index);
}
