#pragma once
#include "FileSearch.h"
namespace aelura::filesearch {
Snapshot queryEverything(std::wstring const& query, Options options, std::function<bool()> const& cancelled);
}
