#pragma once
#include <array>
#include <string>
#include <cwctype>
namespace aelura::catalog {
struct Tool { const wchar_t* id; int category; const wchar_t* title; const wchar_t* description; const wchar_t* keywords; const wchar_t* glyph; const wchar_t* action; };
inline constexpr std::array<Tool, 3> tools{{
    {L"capture", 1, L"截图与标注", L"截取区域或窗口，标注并保存图片。", L"截图 图像 分享 screenshotplus capture png 标注", L"\xE722", L"开始截图"},
    {L"move", 2, L"文件迁移", L"迁移文件或文件夹，保留原路径访问。", L"文件 磁盘 空间 搬家 迁移 链接 freemove squaremove", L"\xE8B7", L"打开迁移"},
    {L"strike", 3, L"StrikeSense", L"管理 CS2 游戏配置、音效与道具工具。", L"游戏 cs2 strikesense gsi steam 音效 道具 配置", L"\xE7FC", L"打开工具"}
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
