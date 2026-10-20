# 测试工程

`AeluraTool.slnx` 包含主程序 `AeluraTool` 和独立控制台测试工程 `CoreTests`，对应 Java 中生产代码与 test 代码的分离。生产代码保留现有目录；测试入口为 `Tests/test_main.cpp`，不会编入主程序。测试直接编译被测核心源码，无需启动或部署 WinUI 主窗口，也无需额外测试框架。

## 运行

在仓库根目录执行（Windows、Visual Studio C++ 工具链、Windows SDK、Python 3）：

```powershell
# 构建测试工程、运行核心测试、检查资源并构建/验证截图通信
.\scripts\test.ps1 -Configuration Debug
.\scripts\test.ps1 -Configuration Release

# 快速运行一个核心模块，不构建和启动截图组件
.\scripts\test.ps1 -Configuration Debug -Suite hash -CoreOnly
.\scripts\test.ps1 -Configuration Debug -Suite migration -CoreOnly
```

也可以在 Visual Studio 中打开解决方案，将 `CoreTests` 设为启动项目后运行或调试。它是控制台测试运行器，测试资源管理器不会自动发现用例。已配置 MSBuild 的终端可通过 `MSBuild Tests\CoreTests.vcxproj /t:RunTests /p:Configuration=Debug /p:Platform=x64` 构建并运行核心测试。

## 目录与扩展

- `RecordingTests.vcxproj`、`recording_tests.cpp`：独立录屏回归，生成 NV12 画面与静音 PCM，验证 CPU / 标准 GPU / NVENC / AMF 编码、实际解码帧数、AAC、MP4 分段合并、取消、已有目标保护、负坐标选区、配置与五分钟保留、恢复清单迁移及原件保留。缺失显卡/编码器时明确输出 LIMIT。默认测试不采集屏幕或麦克风，不操作真实用户文件。
- `RecordingTests.exe --inspect <MP4>`：只读解码并打印尺寸、时长和帧数。`--recover <JSON或缓存目录> <输出目录>`：后台合并恢复片段，保留原片段。只对自动创建的测试目录使用这些测试入口。
- 手工检查辅助：`--pattern` 显示程序生成的测试窗口，三分钟后自动关闭；`--probe-microphone` 短暂读取默认通信麦克风并丢弃采样，只打印格式/样本数，不保存声音。不在默认回归中开启麦克风。
- `test_main.cpp`：测试入口、模块选择、临时目录及退出码。
- `hash_tests.cpp`：哈希向量、双文件比较、路径校验、取消及文件变化保护。
- `migration_tests.cpp`：迁移、取消和冲突保护，以及资源语言、搜索和快捷键失败回滚。
- `check_i18n.py`：中英文资源键及静态引用检查。
- `smoke_capture.py`：截图进程通信和宿主 Job 生命周期检查。
- `CaptureSharingTests.vcxproj`、`image_sharing_tests.cpp`：独立验证未保存图片的 PNG/DIB/DIBV5 像素、临时 PNG 拖放数据、文件保留和缓存清理；不会改写系统剪贴板。完整测试脚本会执行它。

新增测试文件时在 `CoreTests.vcxproj` 添加 `ClCompile`，并在 `test_main.cpp` 注册测试函数；断言失败抛出异常即可。不要用 `assert` 检查关键结果，以免 Release 配置关闭断言。

输出位于忽略目录 `artifacts/tests/<Configuration>/`；退出码 0 表示通过，1 表示测试失败，2 表示运行参数错误。文件测试只操作自动创建且独占的临时目录，成功后清理，失败时保留目录并打印位置。符号链接等环境不支持的检查会打印 `LIMIT`。请勿同时运行多份涉及全局快捷键的测试。

完整程序构建另用 `scripts/build.ps1`。自动测试覆盖搜索和核心设置逻辑，主题外观及分类导航仍需实际窗口验证。
