# 测试工程

`AeluraTool.slnx` 包含主程序 `AeluraTool` 和独立控制台测试工程 `CoreTests`，对应 Java 中生产代码与 test 代码的分离。生产代码保留现有目录；测试入口为 `Tests/test_main.cpp`，不会编入主程序。测试直接编译被测核心源码，无需启动或部署 WinUI 主窗口，也无需额外测试框架。

## 运行

`all` 还包含内置文件索引、junction 不递归、Unicode 与组合搜索、大小写与全字、通配符、扩展名/文件/目录筛选、正则表达式、缓存损坏恢复、创建/重命名/删除后刷新、取消保留旧索引、旧查询丢弃，以及检索快捷键冲突/保存失败回滚测试。所有文件样本、索引根目录及缓存均位于测试自动创建的临时目录，不依赖 Everything。

USN 用合成记录验证新增/删除、目录改名与子路径、损坏记录不发布、日志 ID/有效区间以及重解析点排除，不为测试枚举真实用户卷。权限测试通过 Windows COM 构造但不注册计划任务，验证当前用户、交互登录、最高权限、部署路径与托盘参数，并用拒绝的子进程验证不匹配账户的辅助请求；不弹 UAC，不改变真实自启动设置。SDK 合成服务使用跨进程互斥隔离，避免 Debug / Release 同时运行互相回答缺失服务和超时用例。

WinUI 实机检查可在 Debug 部署目录放置 UTF-8 `file-search-test-root.txt`，内容为本轮新建的临时目录绝对路径。该目录必须直接位于系统临时目录下，名称以 `AeluraSearchUi-` 开头，含 `.aelura-filesearch-fixture` 标记，样本位于其 `files` 子目录；索引缓存位于其 `cache` 子目录。只有 Debug 构建读取此测试入口，Release 忽略它。检查后退出测试进程并删除入口及本轮样本。

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

- `test_main.cpp`：测试入口、模块选择、临时目录及退出码。
- `hash_tests.cpp`：哈希向量、双文件比较、路径校验、取消及文件变化保护。
- `migration_tests.cpp`：迁移、取消和冲突保护，以及资源语言、搜索和快捷键失败回滚。
- `check_i18n.py`：中英文资源键及静态引用检查。
- `smoke_capture.py`：截图进程通信和宿主 Job 生命周期检查。
- `CaptureSharingTests.vcxproj`、`image_sharing_tests.cpp`：独立验证未保存图片的 PNG/DIB/DIBV5 像素、临时 PNG 拖放数据、文件保留和缓存清理；不会改写系统剪贴板。完整测试脚本会执行它。

新增测试文件时在 `CoreTests.vcxproj` 添加 `ClCompile`，并在 `test_main.cpp` 注册测试函数；断言失败抛出异常即可。不要用 `assert` 检查关键结果，以免 Release 配置关闭断言。

输出位于忽略目录 `artifacts/tests/<Configuration>/`；退出码 0 表示通过，1 表示测试失败，2 表示运行参数错误。文件测试只操作自动创建且独占的临时目录，成功后清理，失败时保留目录并打印位置。符号链接等环境不支持的检查会打印 `LIMIT`。请勿同时运行多份涉及全局快捷键的测试。

完整程序构建另用 `scripts/build.ps1`。自动测试覆盖搜索和核心设置逻辑，主题外观及分类导航仍需实际窗口验证。
