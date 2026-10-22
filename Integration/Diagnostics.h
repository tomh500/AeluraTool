#pragma once
#include <windows.h>
#include <string_view>
namespace aelura::diagnostics {
void failure(std::string_view operation, HRESULT code, std::wstring_view message = {}) noexcept;
}
