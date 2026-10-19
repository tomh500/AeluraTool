# 遥汐 / AeluraTool

Windows 桌面工具集合，主界面使用 C++/WinRT 与 WinUI 3。

## 功能

- 文件哈希校验：只读计算本地普通文件的 SHA-256，支持预期值校验、双文件比较和复制摘要。预期值忽略大小写及首尾空白；支持后台进度、取消，切换页面或语言不重启任务。文件任务结束前禁止关闭主窗口，哈希与迁移不能同时运行。首版不支持目录、文件链接、网络路径、数据流、批量清单或在线查询，不保存文件历史。
- 全部工具与图像处理、文件管理、游戏工具分类；支持中文、英文关键词及空格分词搜索。
- 跟随系统、浅色、深色主题，背景、文字、控件和标题栏随主题变化，保存后跨启动恢复。
- 默认按 Windows 显示语言选择界面：中文系统使用中文，其余使用英语；设置中可手动切换并即时生效。主界面、快捷键、迁移提示及截图组件共用中英文资源；StrikeSense 继承宿主语言并保留原有双语实现。
- 原生启动动画：两条潮线与光点，首次或版本更新后播放约 0.85 秒，日常启动仅轻淡入。可在设置关闭或预览，遵循 Windows 动画设置，点击或按键立即跳过。
- 可搜索的快捷键列表：开始截图、最近截图、打开文件迁移、打开 StrikeSense、PrintScreen、打开文件哈希校验。逐项启停和编辑，注册冲突或保存失败时保留旧设置。升级保留原截图按键；哈希入口默认停用，预设 Ctrl+Alt+H。
- 截图与标注：区域、窗口截图，设置及最近截图。截图组件随主程序退出。
- 文件迁移：选择文件或目录，迁移后保留原路径访问；同盘重命名，跨盘复制并逐字节校验。支持空目录、进度、取消、目标冲突保护、失败回滚和恢复信息。目录使用 junction，文件使用符号链接。
- StrikeSense：完整源码集成为独立进程，从游戏分类或快捷键打开。保留现有游戏配置和界面；内部游戏快捷键仍在 StrikeSense 内设置。打开时不自动写入游戏配置，关闭窗口直接退出。主程序退出后已打开的 StrikeSense 可独立使用。

## 构建和运行

完整的编译环境、MSIX 签名打包、安装依赖与二次分发说明见 [BUILD_AND_DISTRIBUTE.md](BUILD_AND_DISTRIBUTE.md)。`scripts/package.ps1` 可生成 Release x64 安装包，默认不签名。

需要 Visual Studio C++/WinUI 工作负载、Windows SDK，以及 packages.config 中的 NuGet 包。StrikeSense 依赖 vcpkg 的 x64-windows-static：nlohmann-json、cpp-httplib（brotli）、yaml-cpp、sdl2、sdl2-mixer、sdl2-ttf、simdjson 及其传递依赖。默认位置为 %USERPROFILE%/vcpkg，可通过 MSBuild 属性 AeluraVcpkgRoot 指定其他路径。

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Configuration Debug
.\scripts\test.ps1 -Configuration Release
.\scripts\test.ps1 -Configuration Debug
.\scripts\deploy.ps1 -Configuration Release
```

也可以在 Visual Studio 打开 AeluraTool.slnx，选择 x64 后生成并部署启动。命令行部署按构建生成的 appxrecipe 更新 AppX 布局，保留应用身份和用户设置；先关闭运行中的遥汐再部署。主程序需要包身份，从开始菜单打开遥汐。仅支持 x64。

构建自动编译 Tools/ScreenShotPlus 和 Tools/StrikeSense，运行时不依赖原项目目录。第三方声明随安装复制到 Tools/Licenses。

## 配置和恢复

主题、语言、启动动画及宿主快捷键位于 %LOCALAPPDATA%/AeluraTool/hotkeys.ini；MSIX 可能重定向至包的 LocalCache/Local。新配置默认跟随系统主题、自动语言并启用动画，保留已有主题选择。截图设置位于同目录 ScreenShotPlus/config.json。StrikeSense 继续使用 %USERPROFILE%/StrikeSense。

迁移前关闭使用原文件的程序。禁止驱动器根目录、Windows 目录、用户和应用数据根目录、重解析点及网络路径；不能覆盖已有目标。目录链接要求支持 junction 的本地文件系统；文件符号链接需要开发者模式或管理员权限，预检失败不会移动原件。迁移过程中主窗口暂不允许关闭。

跨盘先在目标盘暂存，完成内容校验后将原件改名备份，再提交新位置并创建链接。任务恢复信息 .aelura-recovery-{GUID}.txt 写在原件父目录；发生中断时按记录核对原路径、新位置、备份和暂存副本，保留唯一副本。成功后清理备份，清理失败则保留恢复记录并提示。恢复当前为人工核对，不提供自动恢复向导。请勿迁移正在运行的系统服务、数据库或应用。

## 开发

AGENTS.md 约定产品文案、主题、分类注册、快捷键和 Git 工作流。Strings/zh-CN.json 与 Strings/en-US.json 保存共享文案，Integration/I18n 负责读取嵌入资源和语言选择，Tests/check_i18n.py 校验键集合及静态引用；开始菜单文案位于对应的 Resources.resw。Integration/ToolCatalog.h 注册工具；Integration/HotkeyManager 管理宿主快捷键；Integration/FileMigration 为独立文件迁移核心。测试只在自动创建的临时目录操作，不迁移用户文件。

本地 Git 已建立初始快照和分功能提交，没有添加远程仓库。第三方来源与集成说明见 Tools/StrikeSense/ORIGIN.md、Tools/ScreenShotPlus/THIRD_PARTY_NOTICES.md。文件迁移为新的独立实现，未复制原 FreeMove_Square 源码。
