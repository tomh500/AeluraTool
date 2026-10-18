#include "hotkeys.h"
using namespace std;
// The managed capture engine never owns global hotkeys or startup entries.
wstring RegisterConfiguredHotkeys(HWND, Hotkeys&, const Config&) { return {}; }
void ReleaseHotkeys(HWND, const Hotkeys&) {}
bool ApplyConfiguredSettings(HWND, Hotkeys&, Config& current, const Config& next,
    const filesystem::path& file, wstring& error, const wchar_t*, const filesystem::path& fallback, filesystem::path* savedFile) {
    Config candidate = next;
    candidate.modifiers = current.modifiers; candidate.key = current.key;
    candidate.printScreen = false; candidate.startup = false;
    if (!ValidConfig(candidate)) { error = tr("screen.hotkeys.001"); return false; }
    filesystem::path destination;
    if (!SaveConfigWithFallback(file, fallback, candidate, destination, error)) return false;
    current = candidate; if (savedFile) *savedFile = move(destination); return true;
}