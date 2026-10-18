#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <cstdint>
namespace aelura::migration {
struct Plan { std::filesystem::path source, destination; bool directory{}, sameVolume{}; uintmax_t bytes{}; };
struct Result { bool success{}; std::wstring message; std::filesystem::path recovery; };
using Progress = std::function<bool(uintmax_t, uintmax_t)>;
Plan inspect(std::filesystem::path source, std::filesystem::path destinationFolder);
Result execute(Plan const& plan, Progress const& progress, bool copyAlways = false);
}
