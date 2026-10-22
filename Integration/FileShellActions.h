#pragma once
#include "FileSearch.h"
#include <shobjidl.h>
namespace aelura::files {
using Path = std::filesystem::path;
enum class Kind { Paste, Rename, Recycle, Shortcut };
struct Transfer { std::vector<Path> paths; bool move{}; };
struct Request {
    Kind kind{}; std::vector<Path> sources; Path destination; std::wstring name; bool move{};
    HWND owner{}; bool interactive{true}, verifyMove{}; Path recoveryFolder;
    std::shared_ptr<filesearch::Index> index;
};
struct State { uint64_t revision{}; bool busy{}, cancelled{}, failed{}; unsigned completed{}, total{}; Path recovery; };
bool validName(std::wstring const& name);
bool protectedPath(Path const& path);
HRESULT preflight(Request const& request, std::function<bool()> const& cancelled = {});
HRESULT dataObject(Path const& source, bool cut, IDataObject** result);
Transfer readTransfer(IDataObject* object);
HRESULT clipboard(Path const& source, bool cut);
Request pasteRequest(Path const& destination, HWND owner);
bool properties(Path const& path, HWND owner);
bool start(Request request);
State state();
bool active();
void dialog(bool open);
void cancel();
void completeClipboardMove(HWND owner);
}
