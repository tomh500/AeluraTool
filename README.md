# 遥汐工具箱 / AeluraTool

C++/WinRT + WinUI 3 系统工具箱，首版集成 ScreenShotPlus。

在 Visual Studio 中打开 AeluraTool.slnx，选择 x64 / Release，生成并运行。需要已安装 Windows App SDK、C++ WinUI 工作负载和 Windows SDK。构建会自动从 Tools/ScreenShotPlus 源码编译托管截图组件，并复制到主程序的 Tools 目录及 MSIX 布局中。

## 使用

- 工具百宝箱：开始截图、截图设置、最近截图、搜索。截图前工具箱最小化，可从任务栏恢复。
- 全局快捷键：默认 Ctrl+Alt+A，支持 Ctrl/Alt/Shift + 字母、数字、F1–F24，可选 PrintScreen；保存前检查占用，失败保留原注册。
- 配置：%LOCALAPPDATA%/AeluraTool/hotkeys.ini；截图配置独立保存在同目录的 ScreenShotPlus/config.json。
- 关闭工具箱会释放快捷键并结束其截图组件。要继续使用全局快捷键，请最小化工具箱。

## 集成约定

工具来源可以是源码、外部 EXE 或自研功能。当前第一件工具以本仓库内的 MIT 源码副本编译为受管子进程，主界面为 WinUI，截图及标注界面保留原实现。原 ScreenShotPlus 仓库、原 EXE 及其配置不受修改。集成组件不注册全局快捷键，不提供快捷键和自启动编辑入口；由主进程统一注册和派发截图命令。进程加入关闭即退出的 Job Object，避免工具箱退出后遗留组件。

Windows 消息协议：WM_APP+120，wParam 1=截图，2=设置，3=最近截图，4=退出，5=检查是否可截图。成功返回 1，忙碌返回 2，未知命令返回 0；协议定义在 Integration/CaptureProtocol.h。仅向实际启动的子进程 PID 且带就绪标识的窗口发送命令。

源码位于 Tools/ScreenShotPlus，许可证及第三方声明保留；nlohmann/json 头文件副本位于 include/nlohmann，遵循其 MIT 许可证。

MSIX 调试部署会将 LocalAppData 的文件访问重定向到该包的 LocalCache/Local/AeluraTool；配置仍由同一应用身份跨启动保存。

## 命令行构建与验证

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Configuration Debug
python Tests\smoke_capture.py Release
python Tests\smoke_capture.py Debug
```

主程序为带包身份的 WinUI 应用，使用 Visual Studio F5 部署启动；构建目录的裸 EXE 不能替代包激活。截图组件随主程序部署，不单独启动。自动检查不模拟鼠标键盘、不生成截图、不修改截图设置。

首版验收记录见 VERIFICATION.md。第三方源码基于本机 ScreenShotPlus 仓库提交 c73498b0c4ddf4b71478dd3ba9f188c9482f5211 的工作区快照，在本仓库中维护托管集成修改。
