#include "FileSearch.h"
#include "NtfsFileIndex.h"
#include "EverythingSearch.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#define _REGEX_MAX_COMPLEXITY_COUNT 100000L
#define _REGEX_MAX_STACK_COUNT 1000L
#include <regex>
namespace aelura::filesearch {
namespace {
constexpr size_t BatchSize = 2048;
constexpr uint64_t CacheMagic = 0x324958444E494641ULL;
std::wstring fold(std::wstring value) {
    if (value.empty()) return value;
    std::wstring normalized(value.size(), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()),
        normalized.data(), static_cast<int>(normalized.size()), nullptr, nullptr, 0)) return normalized;
    for (auto& c : value) c = static_cast<wchar_t>(towlower(c)); return value;
}
Entry entry(std::wstring path, bool folder) {
    auto name = std::filesystem::path(path).filename().wstring();
    // Most searches use names. Avoid keeping a second full path for every file.
    return {{name, std::move(path), folder}, fold(name)};
}
std::vector<std::filesystem::path> drives() {
    std::vector<std::filesystem::path> roots; wchar_t buffer[512]{};
    auto length = GetLogicalDriveStringsW(512, buffer); if (!length || length >= 512) return roots;
    for (auto at = buffer; *at; at += wcslen(at) + 1) {
        auto type = GetDriveTypeW(at);
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) roots.emplace_back(at);
    }
    return roots;
}
struct FindHandle { HANDLE value{INVALID_HANDLE_VALUE}; ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); } };
bool word(wchar_t c) { WORD type{}; return c == L'_' || (GetStringTypeW(CT_CTYPE1, &c, 1, &type) && (type & (C1_ALPHA | C1_DIGIT))); }
bool substring(std::wstring const& text, std::wstring const& term, bool whole) {
    for (auto at = text.find(term); at != std::wstring::npos; at = text.find(term, at + 1))
        if (!whole || ((at == 0 || !word(text[at - 1])) && (at + term.size() == text.size() || !word(text[at + term.size()])))) return true;
    return false;
}
bool glob(std::wstring const& text, std::wstring const& pattern) {
    size_t p = 0, t = 0, star = std::wstring::npos, retry = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == L'?' || pattern[p] == text[t])) { ++p; ++t; }
        else if (p < pattern.size() && pattern[p] == L'*') { star = p++; retry = t; }
        else if (star != std::wstring::npos) { p = star + 1; t = ++retry; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == L'*') ++p;
    return p == pattern.size();
}
bool readString(std::istream& input, std::wstring& value) {
    uint32_t length{}; input.read(reinterpret_cast<char*>(&length), sizeof(length));
    if (!input || !length || length > 32767) return false;
    value.resize(length); input.read(reinterpret_cast<char*>(value.data()), length * sizeof(wchar_t));
    return input.good() && value.find(L'\0') == std::wstring::npos;
}
void writeString(std::ostream& output, std::wstring const& value) {
    auto length = static_cast<uint32_t>(value.size()); output.write(reinterpret_cast<char const*>(&length), sizeof(length));
    output.write(reinterpret_cast<char const*>(value.data()), length * sizeof(wchar_t));
}
}
Index::Index(std::filesystem::path cache, std::vector<std::filesystem::path> roots) : cache_(std::move(cache)),
    roots_(roots.empty() ? drives() : std::move(roots)), worker_([this] { run(); }) {
    auto settings = cache_; settings += L".ini";
    everything_ = !cache_.empty() && GetPrivateProfileIntW(L"Search", L"EverythingSDK", 0, settings.c_str()) == 1;
    if (!everything_) refresh();
}
Index::~Index() { stopping_ = true; cancel_ = true; wake_.notify_one(); if (worker_.joinable()) worker_.join(); }
void Index::refresh() {
    std::lock_guard lock(mutex_); if (data_.progress.running || everything_) return;
    cancel_ = false; pending_ = true; data_.progress.running = true;
    data_.progress.cancelled = data_.progress.failed = data_.progress.cacheFailed = false; ++data_.progress.revision; wake_.notify_one();
}
void Index::cancel() { std::lock_guard lock(mutex_); restartBuiltin_ = false; cancel_ = true; }
Progress Index::progress() const { std::lock_guard lock(mutex_); return data_.progress; }
IndexSnapshot Index::snapshot() const { std::lock_guard lock(mutex_); return data_; }
bool Index::setEverything(bool enabled) {
    if (everything_ == enabled) return true;
    if (!cache_.empty()) {
        std::error_code error; std::filesystem::create_directories(cache_.parent_path(), error);
        auto settings = cache_; settings += L".ini";
        if (error || !WritePrivateProfileStringW(L"Search", L"EverythingSDK", enabled ? L"1" : L"0", settings.c_str())) return false;
    }
    everything_ = enabled;
    {
        std::lock_guard lock(mutex_); ++data_.progress.revision;
        if (enabled) { restartBuiltin_ = false; if (data_.progress.running) cancel_ = true; }
        else if (data_.progress.running && cancel_) restartBuiltin_ = true;
    }
    if (!enabled && !progress().running && (progress().indexed == 0 || progress().cancelled)) refresh();
    return true;
}
void Index::publish(std::shared_ptr<Batch> const& batch, uint64_t skipped) {
    std::lock_guard lock(mutex_);
    if (!batch->empty()) { data_.batches.push_back(batch); data_.progress.indexed += batch->size(); }
    data_.progress.skipped = skipped; ++data_.progress.revision;
}
bool Index::load() {
    if (cache_.empty() || cancel_ || stopping_) return false;
    std::ifstream input(cache_, std::ios::binary); if (!input) return false;
    uint64_t magic{}, count{}; input.read(reinterpret_cast<char*>(&magic), 8); input.read(reinterpret_cast<char*>(&count), 8);
    // Bound allocations by the actual cache length, not an arbitrary file-count ceiling.
    std::error_code ec; auto bytes = std::filesystem::file_size(cache_, ec);
    if (!input || magic != CacheMagic || ec || bytes < 16 || count > (bytes - 16) / 7) return false;
    uint32_t rootCount{}; input.read(reinterpret_cast<char*>(&rootCount), sizeof(rootCount));
    if (!input || rootCount != roots_.size()) return false;
    for (auto const& root : roots_) { std::wstring saved; if (!readString(input, saved) || saved != root.wstring()) return false; }
    IndexSnapshot cached; cached.progress.cached = true;
    input.read(reinterpret_cast<char*>(&cached.progress.skipped), sizeof(cached.progress.skipped)); if (!input) return false;
    auto batch = std::make_shared<Batch>(); batch->reserve(BatchSize);
    for (uint64_t i = 0; i < count; ++i) {
        if (cancel_ || stopping_) return false;
        std::wstring path; unsigned char folder{};
        if (!readString(input, path) || !std::filesystem::path(path).is_absolute()) return false;
        input.read(reinterpret_cast<char*>(&folder), 1); if (!input || folder > 1) return false;
        batch->push_back(entry(std::move(path), folder != 0));
        if (batch->size() == BatchSize) { cached.batches.push_back(batch); batch = std::make_shared<Batch>(); batch->reserve(BatchSize); }
    }
    if (!batch->empty()) cached.batches.push_back(batch);
    if (input.peek() != std::char_traits<char>::eof()) return false;
    cached.progress.indexed = count;
    std::lock_guard lock(mutex_); cached.progress.revision = data_.progress.revision + 1; cached.progress.running = true; data_ = std::move(cached);
    return true;
}
bool Index::save() {
    if (cache_.empty() || cancel_ || stopping_) return true;
    auto data = snapshot(); if (data.progress.failed || data.progress.cancelled) return true;
    std::error_code ec; std::filesystem::create_directories(cache_.parent_path(), ec); if (ec) return false;
    auto temporary = cache_; temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc); const uint64_t magic = CacheMagic;
    output.write(reinterpret_cast<char const*>(&magic), 8); output.write(reinterpret_cast<char const*>(&data.progress.indexed), 8);
    auto rootCount = static_cast<uint32_t>(roots_.size()); output.write(reinterpret_cast<char const*>(&rootCount), sizeof(rootCount));
    for (auto const& root : roots_) writeString(output, root.wstring());
    output.write(reinterpret_cast<char const*>(&data.progress.skipped), sizeof(data.progress.skipped));
    for (auto const& batch : data.batches) for (auto const& value : *batch) {
        if (cancel_ || stopping_) { output.close(); DeleteFileW(temporary.c_str()); return true; }
        writeString(output, value.item.path); const unsigned char folder = value.item.folder ? 1 : 0; output.write(reinterpret_cast<char const*>(&folder), 1);
    }
    output.flush(); const bool ok = output.good(); output.close();
    if (cancel_ || stopping_) { DeleteFileW(temporary.c_str()); return true; }
    if (!ok || !MoveFileExW(temporary.c_str(), cache_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(temporary.c_str()); return false; }
    return true;
}
void Index::build() {
    auto previous = snapshot(); bool replaced = false; uint64_t skipped = 0;
    const bool retain = previous.progress.cached || !previous.batches.empty();
    std::vector<std::shared_ptr<Batch const>> staged; uint64_t stagedCount = 0;
    auto batch = std::make_shared<Batch>(); batch->reserve(BatchSize);
    auto flush = [&] {
        if (retain) {
            stagedCount += batch->size(); if (!batch->empty()) staged.push_back(batch);
            batch = std::make_shared<Batch>(); batch->reserve(BatchSize); return;
        }
        if (!replaced) {
            std::lock_guard lock(mutex_); data_.batches.clear(); data_.progress.indexed = 0; data_.progress.cached = false;
            data_.progress.skipped = 0; replaced = true;
        }
        publish(batch, skipped); batch = std::make_shared<Batch>(); batch->reserve(BatchSize);
    };
    auto pending = roots_;
    while (!pending.empty() && !cancel_ && !stopping_) {
        auto directory = std::move(pending.back()); pending.pop_back();
        if (ntfs::enumerate(directory, [&](std::wstring path, bool folder) {
            batch->push_back(entry(std::move(path), folder));
            if (batch->size() == BatchSize) flush();
        }, [&] { return cancel_ || stopping_; })) continue;
        auto attributes = GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { ++skipped; continue; }
        WIN32_FIND_DATAW found{}; FindHandle handle; auto pattern = directory / L"*";
        handle.value = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (handle.value == INVALID_HANDLE_VALUE) { if (GetLastError() != ERROR_FILE_NOT_FOUND) ++skipped; continue; }
        DWORD error = ERROR_NO_MORE_FILES;
        do {
            if (cancel_ || stopping_) break;
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
            auto path = directory / found.cFileName; const bool folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            batch->push_back(entry(path.wstring(), folder));
            if (folder && !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) pending.push_back(std::move(path));
            if (batch->size() == BatchSize) flush();
            SetLastError(ERROR_NO_MORE_FILES);
        } while (FindNextFileW(handle.value, &found));
        error = GetLastError(); if (error != ERROR_NO_MORE_FILES && !cancel_ && !stopping_) ++skipped;
    }
    if (!batch->empty() || !replaced) flush();
    {
        std::lock_guard lock(mutex_);
        if (cancel_ && !previous.batches.empty()) {
            data_.batches = std::move(previous.batches); data_.progress.indexed = previous.progress.indexed; data_.progress.cached = previous.progress.cached;
        } else if (retain && !cancel_ && !stopping_) {
            data_.batches = std::move(staged); data_.progress.indexed = stagedCount; data_.progress.cached = false;
        }
        data_.progress.skipped = skipped; data_.progress.cancelled = cancel_; ++data_.progress.revision;
    }
    auto saved = save();
    std::lock_guard lock(mutex_); data_.progress.cacheFailed = !saved;
}
void Index::run() {
    while (!stopping_) {
        { std::unique_lock lock(mutex_); wake_.wait(lock, [this] { return stopping_ || pending_; }); if (stopping_) break; pending_ = false; }
        try {
            bool reused = false;
            if (!loaded_) { reused = load(); loaded_ = true; }
            if (!reused) build();
        }
        catch (...) { std::lock_guard lock(mutex_); data_.progress.failed = true; }
        std::lock_guard lock(mutex_);
        if (restartBuiltin_ && !everything_) { restartBuiltin_ = false; pending_ = true; cancel_ = false; data_.progress.running = true; data_.progress.cancelled = false; }
        else data_.progress.running = false;
        ++data_.progress.revision;
    }
}
void preserveResultOrder(std::vector<Item> const& previous, std::vector<Item>& incoming) {
    std::vector<Item> stable; stable.reserve(incoming.size());
    for (auto const& old : previous) {
        auto at = std::find_if(incoming.begin(), incoming.end(), [&](auto const& item) { return item.path == old.path; });
        if (at != incoming.end()) { stable.push_back(std::move(*at)); incoming.erase(at); }
    }
    for (auto& item : incoming) stable.push_back(std::move(item));
    incoming = std::move(stable);
}
Snapshot match(IndexSnapshot const& source, std::wstring const& query, Options options, std::function<bool()> const& cancelled) {
    Snapshot result; result.sourceRevision = source.progress.revision; result.state = State::Ready;
    if (query.empty()) { result.state = State::Idle; return result; }
    if (query.size() > 512) { result.state = State::InvalidQuery; return result; }
    if (cancelled && cancelled()) { result.state = State::Idle; return result; }
    const bool usePath = options.path || (!options.regex && query.find_first_of(L"\\/") != std::wstring::npos);
    struct Term { std::wstring value; int type{}; bool wildcard{}; }; std::vector<Term> terms; std::wregex expression;
    try {
        if (options.regex) expression = std::wregex(query, std::regex_constants::ECMAScript | (options.matchCase ? std::regex_constants::syntax_option_type{} : std::regex_constants::icase));
        else {
            std::wstring token; bool quote = false;
            auto add = [&] {
                if (token.empty()) return;
                Term term; auto lowered = fold(token);
                if (lowered.rfind(L"ext:", 0) == 0) { term.type = 1; token.erase(0, 4); }
                else if (lowered.rfind(L"folder:", 0) == 0) { term.type = 2; token.erase(0, 7); }
                else if (lowered.rfind(L"file:", 0) == 0) { term.type = 3; token.erase(0, 5); }
                term.value = options.matchCase ? token : fold(token); term.wildcard = term.value.find_first_of(L"*?") != std::wstring::npos;
                terms.push_back(std::move(term)); token.clear();
            };
            for (auto c : query) { if (c == L'"') quote = !quote; else if (iswspace(c) && !quote) add(); else token += c; }
            if (quote) { result.state = State::InvalidQuery; return result; } add();
        }
        size_t checked = 0;
        for (auto const& batch : source.batches) for (auto const& value : *batch) {
            if (++checked % 64 == 0 && cancelled && cancelled()) { result.state = State::Idle; result.items.clear(); return result; }
            auto foldedPath = usePath && !options.matchCase ? fold(value.item.path) : std::wstring{};
            auto const& target = usePath ? (options.matchCase ? value.item.path : foldedPath) : (options.matchCase ? value.item.name : value.foldedName);
            bool accepted = true;
            if (options.regex) {
                auto const& text = options.path ? value.item.path : value.item.name;
                if (options.wholeWord) {
                    std::wsregex_iterator at(text.begin(), text.end(), expression), end; accepted = false;
                    for (; at != end; ++at) {
                        auto pos = static_cast<size_t>(at->position()), len = static_cast<size_t>(at->length());
                        if ((pos == 0 || !word(text[pos - 1])) && (pos + len == text.size() || !word(text[pos + len]))) { accepted = true; break; }
                    }
                } else accepted = std::regex_search(text, expression);
            } else for (auto const& term : terms) {
                if ((term.type == 2 && !value.item.folder) || (term.type == 3 && value.item.folder)) { accepted = false; break; }
                if (term.type == 1) {
                    auto dot = value.item.name.find_last_of(L'.'); std::wstring extension = dot == std::wstring::npos ? L"" : value.item.name.substr(dot + 1);
                    if (!options.matchCase) extension = fold(extension);
                    bool found = false; size_t start = 0;
                    do { auto end = term.value.find(L';', start); if (!value.item.folder && extension == term.value.substr(start, end - start)) found = true; if (end == std::wstring::npos) break; start = end + 1; } while (start <= term.value.size());
                    if (!found) { accepted = false; break; }
                } else if (!term.value.empty() && !(term.wildcard ? glob(target, options.wholeWord ? term.value : L"*" + term.value + L"*") : substring(target, term.value, options.wholeWord))) { accepted = false; break; }
            }
            if (accepted) { ++result.total; if (result.items.size() < ResultLimit) result.items.push_back(value.item); }
        }
        std::sort(result.items.begin(), result.items.end(), [](auto const& a, auto const& b) { return fold(a.name) < fold(b.name); });
    } catch (std::regex_error const&) { result = {}; result.state = State::InvalidQuery; }
    catch (...) { result = {}; result.state = State::Failed; }
    result.sourceRevision = source.progress.revision;
    return result;
}
Session::Session(std::shared_ptr<Index> index) : index_(std::move(index)), worker_([this] { run(); }) {}
Session::~Session() { stopping_ = true; ++generation_; wake_.notify_one(); if (worker_.joinable()) worker_.join(); }
uint64_t Session::search(std::wstring query, Options options) {
    std::lock_guard lock(mutex_); auto id = ++generation_; query_ = std::move(query); options_ = options;
    pending_ = !query_.empty(); result_ = {}; result_.generation = id; result_.state = pending_ ? State::Searching : State::Idle; wake_.notify_one(); return id;
}
void Session::cancel() { search(L""); }
Snapshot Session::snapshot() const { std::lock_guard lock(mutex_); return result_; }
void Session::run() {
    while (!stopping_) {
        uint64_t id; std::wstring query; Options options;
        { std::unique_lock lock(mutex_); wake_.wait(lock, [this] { return stopping_ || pending_; }); if (stopping_) break; id = generation_; query = query_; options = options_; pending_ = false; }
        Snapshot result;
        try {
            auto cancelled = [this, id] { return stopping_ || generation_ != id; };
            if (index_->everything()) { result = queryEverything(query, options, cancelled); result.sourceRevision = index_->progress().revision; }
            else result = match(index_->snapshot(), query, options, cancelled);
        }
        catch (...) { result.state = State::Failed; }
        result.generation = id; std::lock_guard lock(mutex_); if (!stopping_ && generation_ == id) result_ = std::move(result);
    }
}
}
