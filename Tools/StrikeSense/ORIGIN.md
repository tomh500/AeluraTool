# StrikeSense 集成副本

来源：用户提供的本地 StrikeSense 项目（2026-10-19）。源项目未修改。
保留完整功能，以独立进程从游戏工具分类启动。配置继续使用 %USERPROFILE%/StrikeSense。

集成变更：移除 Debugger 产品入口和日志重定向；添加主窗口识别；宿主打开时不自动写入游戏配置；关闭窗口直接退出。原游戏内部快捷键仍由 StrikeSense 管理，宿主快捷键仅管理工具唤起。

依赖遵循各自许可，见 Tools/Licenses/StrikeSense。原项目 vcpkg.json 标注 MIT，但没有随附项目 LICENSE；分发前应由项目所有者确认授权。
