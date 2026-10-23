#pragma once
#include <array>
namespace aelura::search {
struct Entry { wchar_t const* id; char const* title; wchar_t const* keywords; int page, category; };
inline constexpr std::array<Entry, 12> settings{{
    {L"account", "account.title", L"账号 帐号 登录 绑定 微软 QQ Microsoft account login sign in bind", 2, 0},
    {L"images", "nav.images", L"图像 截图 images screenshots", 0, 1},
    {L"files", "nav.files", L"文件 files storage", 0, 2},
    {L"games", "nav.games", L"游戏 games gaming", 0, 3},
    {L"keys", "nav.keys", L"快捷键 热键 keyboard shortcuts hotkeys", 1, 0},
    {L"appearance", "settings.theme", L"主题 外观 颜色 深色 浅色 跟随系统 theme appearance dark light system", 2, 0},
    {L"language", "settings.language", L"语言 中文 英语 language chinese english", 2, 0},
    {L"motion", "settings.motion", L"动画 启动 预览 motion animation startup preview", 2, 0},
    {L"close", "close.action", L"关闭 退出 托盘 询问 close exit tray ask", 2, 0},
    {L"autostart", "startup.auto", L"开机 自启动 自动 启动 startup autostart login", 2, 0},
    {L"privileges", "privilege.auto", L"管理员 权限 自动 提权 administrator elevation UAC", 2, 0},
    {L"licenses", "settings.licenses", L"许可 协议 licenses notices", 2, 0}
}};
}
