#pragma once
#include <filesystem>
#include <string>
#include <winrt/Windows.Data.Json.h>
namespace aelura::account {
struct Error { std::string key; };
std::wstring normalizeEndpoint(std::wstring const& value);
bool opaque(std::string const& value);
winrt::Windows::Data::Json::JsonObject request(std::wstring const& endpoint, wchar_t const* method,
    std::wstring const& path, std::string const& bearer = {}, std::string const& session = {});
std::string loadCredential(std::wstring const& endpoint);
void saveCredential(std::wstring const& endpoint, std::string const& token);
void forgetCredential(std::wstring const& endpoint);
bool authorizationUrl(std::wstring const& value, std::string const& provider);
void validateProfile(winrt::Windows::Data::Json::JsonObject const& value);
}
