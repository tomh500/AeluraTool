#include "FileSearch.h"
#include <algorithm>
#include <cwctype>
#include <fstream>
#define _REGEX_MAX_COMPLEXITY_COUNT 100000L
#define _REGEX_MAX_STACK_COUNT 1000L
#include <regex>
namespace aelura::filesearch {
namespace {
constexpr uint64_t MaxEntries = 2000000;
constexpr size_t BatchSize = 2048;
std::wstring fold(std::wstring value) {
    if (value.empty()) return value;
    std::wstring normalized(value.size(), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()),
        normalized.data(), static_cast<int>(normalized.size()), nullptr, nullptr, 0)) return normalized;
    for (auto& c : value) c = static_cast<wchar_t>(towlower(c)); return value;
}
Entry entry(std::wstring path, bool folder) {
    auto name = std::filesystem::path(path).filename().wstring();
    return {{name, path, folder}, fold(name), fold(path)};
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
    roots_(roots.empty() ? drives() : std::move(roots)), worker_([this] { run(); }) { refresh(); }
Index::~Index() { stopping_ = true; cancel_ = true; wake_.notify_one(); if (worker_.joinable()) worker_.join(); }
void Index::refresh() {
    std::lock_guard lock(mutex_); if (data_.progress.running) return;
    cancel_ = false; pending_ = true; data_.progress.running = true;
    data_.progress.cancelled = data_.progress.failed = false; ++data_.progress.revision; wake_.notify_one();
}
void Index::cancel() { cancel_ = true; }
Progress Index::progress() const { std::lock_guard lock(mutex_); return data_.progress; }
IndexSnapshot Index::snapshot() const { std::lock_guard lock(mutex_); return data_; }
void Index::publish(std::shared_ptr<Batch> const& batch, uint64_t skipped) {
    std::lock_guard lock(mutex_);
    if (!batch->empty()) { data_.batches.push_back(batch); data_.progress.indexed += batch->size(); }
    data_.progress.skipped = skipped; ++data_.progress.revision;
}
void Index::load() {
    if (cache_.empty() || cancel_ || stopping_) return;
    std::ifstream input(cache_, std::ios::binary); if (!input) return;
    uint64_t magic{}, count{}; input.read(reinterpret_cast<char*>(&magic), 8); input.read(reinterpret_cast<char*>(&count), 8);
    if (!input || magic != 0x314958444E494641ULL || count > MaxEntries) return;
    IndexSnapshot cached; cached.progress.cached = true;
    auto batch = std::make_shared<Batch>(); batch->reserve(BatchSize);
    for (uint64_t i = 0; i < count; ++i) {
        if (cancel_ || stopping_) return;
        std::wstring path; unsigned char folder{};
        if (!readString(input, path) || !std::filesystem::path(path).is_absolute()) return;
        input.read(reinterpret_cast<char*>(&folder), 1); if (!input || folder > 1) return;
        batch->push_back(entry(std::move(path), folder != 0));
        if (batch->size() == BatchSize) { cached.batches.push_back(batch); batch = std::make_shared<Batch>(); batch->reserve(BatchSize); }
    }
    if (!batch->empty()) cached.batches.push_back(batch);
    cached.progress.indexed = count;
    std::lock_guard lock(mutex_); cached.progress.revision = data_.progress.revision + 1; cached.progress.running = true; data_ = std::move(cached);
}
void Index::save() {
    if (cache_.empty() || cancel_ || stopping_) return;
    auto data = snapshot(); if (data.progress.limited || data.progress.failed || data.progress.cancelled) return;
    std::error_code ec; std::filesystem::create_directories(cache_.parent_path(), ec); if (ec) return;
    auto temporary = cache_; temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc); const uint64_t magic = 0x314958444E494641ULL;
    output.write(reinterpret_cast<char const*>(&magic), 8); output.write(reinterpret_cast<char const*>(&data.progress.indexed), 8);
    for (auto const& batch : data.batches) for (auto const& value : *batch) {
        if (cancel_ || stopping_) { output.close(); DeleteFileW(temporary.c_str()); return; }
        writeString(output, value.item.path); const unsigned char folder = value.item.folder ? 1 : 0; output.write(reinterpret_cast<char const*>(&folder), 1);
    }
    output.flush(); const bool ok = output.good(); output.close();
    if (!ok || cancel_ || stopping_ || !MoveFileExW(temporary.c_str(), cache_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) DeleteFileW(temporary.c_str());
}
void Index::build() {
    auto previous = snapshot(); bool replaced = false; uint64_t skipped = 0, count = 0;
    auto batch = std::make_shared<Batch>(); batch->reserve(BatchSize);
    auto flush = [&] {
        if (!replaced) {
            std::lock_guard lock(mutex_); data_.batches.clear(); data_.progress.indexed = 0; data_.progress.cached = false;
            data_.progress.skipped = 0; data_.progress.limited = false; replaced = true;
        }
        publish(batch, skipped); batch = std::make_shared<Batch>(); batch->reserve(BatchSize);
    };
    auto pending = roots_;
    while (!pending.empty() && !cancel_ && !stopping_ && count < MaxEntries) {
        auto directory = std::move(pending.back()); pending.pop_back();
        auto attributes = GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) { ++skipped; continue; }
        WIN32_FIND_DATAW found{}; FindHandle handle; auto pattern = directory / L"*";
        handle.value = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (handle.value == INVALID_HANDLE_VALUE) { if (GetLastError() != ERROR_FILE_NOT_FOUND) ++skipped; continue; }
        DWORD error = ERROR_NO_MORE_FILES;
        do {
            if (cancel_ || stopping_ || count >= MaxEntries) break;
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
            auto path = directory / found.cFileName; const bool folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            batch->push_back(entry(path.wstring(), folder)); ++count;
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
        }
        data_.progress.skipped = skipped; data_.progress.cancelled = cancel_; data_.progress.limited = count >= MaxEntries; ++data_.progress.revision;
    }
    save();
}
void Index::run() {
    while (!stopping_) {
        { std::unique_lock lock(mutex_); wake_.wait(lock, [this] { return stopping_ || pending_; }); if (stopping_) break; pending_ = false; }
        try { if (!loaded_) { load(); loaded_ = true; } build(); }
        catch (...) { std::lock_guard lock(mutex_); data_.progress.failed = true; }
        std::lock_guard lock(mutex_); data_.progress.running = false; ++data_.progress.revision;
    }
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
            auto const& target = usePath ? (options.matchCase ? value.item.path : value.foldedPath) : (options.matchCase ? value.item.name : value.foldedName);
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
        try { result = match(index_->snapshot(), query, options, [this, id] { return stopping_ || generation_ != id; }); }
        catch (...) { result.state = State::Failed; }
        result.generation = id; std::lock_guard lock(mutex_); if (!stopping_ && generation_ == id) result_ = std::move(result);
    }
}
}
