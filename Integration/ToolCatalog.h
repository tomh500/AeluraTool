#pragma once
#include <array>
#include <string>
#include <cwctype>
namespace aelura::catalog {
struct Tool { const wchar_t* id; int category; const char* title; const char* description; const wchar_t* keywords; const wchar_t* glyph; const char* action; };
inline constexpr std::array<Tool, 7> tools{{
    {L"autoclicker", 3, "autoclicker.title", "autoclicker.description", L"连点 鼠标 长按 游戏 autoclicker auto click rapid fire mouse hold", L"\xE962", "autoclicker.open"},
    {L"record", 1, "record.title", "record.description", L"录屏 录像 录制 视频 重放 回放 五分钟 screen recording recorder replay video h264 h265 nvenc amf", L"\xE714", "record.open"},
    {L"filesearch", 2, "filesearch.title", "filesearch.description", L"快捷 文件 检索 搜索 查找 everything file search find path 文件名 路径", L"\xE721", "filesearch.open"},
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
