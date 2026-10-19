#pragma once
#include <array>
#include <string>
#include <cwctype>
namespace aelura::catalog {
struct Tool { const wchar_t* id; int category; const char* title; const char* description; const wchar_t* keywords; const wchar_t* glyph; const char* action; };
inline constexpr std::array<Tool, 4> tools{{
    {L"hash", 2, "hash.title", "hash.description", L"文件 哈希 校验 比较 摘要 sha256 sha-256 checksum hash verify compare integrity", L"\xE9D9", "hash.open"},
    {L"capture", 1, "capture.title", "capture.description", L"截图 图像 分享 screenshotplus capture png annotation image screenshot", L"\xE722", "capture.start"},
    {L"move", 2, "move.title", "move.card_description", L"文件 磁盘 空间 搬家 迁移 链接 freemove squaremove file folder migration disk link storage", L"\xE8B7", "move.open"},
    {L"strike", 3, "strike.title", "strike.description", L"游戏 cs2 strikesense gsi steam 音效 道具 配置 gaming game sound settings", L"\xE7FC", "strike.open"}
}};
inline std::wstring lower(std::wstring value) { for (auto& c : value) c = static_cast<wchar_t>(towlower(c)); return value; }
inline bool matches(std::wstring text, std::wstring query) {
    text = lower(text); query = lower(query);
    size_t start = 0;
    while (start < query.size()) {
        auto end = query.find(L' ', start); if (end == std::wstring::npos) end = query.size();
        if (end > start && text.find(query.substr(start, end - start)) == std::wstring::npos) return false;
        start = end + 1;
    }
    return true;
}
}
