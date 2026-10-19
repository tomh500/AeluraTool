# 编译、打包和二次分发

当前工程只支持 Windows x64，需要包身份。不要只发送 AeluraTool.exe，也不要把本机注册的 AppX 目录当成免安装版本。

## 1. 准备编译环境

- 本轮实际验证的是 Visual Studio 2026（18.x）、MSVC v145、Windows SDK 与 C++/WinUI 开发组件。工程有 v143 条件配置，但现有脚本固定传入 VisualStudioVersion=18.0，本轮没有验证 VS 2022。
- 在 Visual Studio 打开 `AeluraTool.slnx`，还原 `packages.config` 指定的 NuGet 包。不要将 packages 缓存提交到仓库。
- 安装 vcpkg 依赖，默认位置为 `%USERPROFILE%\vcpkg`。在仓库根目录执行：

```powershell
& "$env:USERPROFILE\vcpkg\vcpkg.exe" install nlohmann-json 'cpp-httplib[brotli]' yaml-cpp sdl2 sdl2-mixer sdl2-ttf simdjson --triplet x64-windows-static
```

依赖还原需网络；仓库尚未固定 vcpkg baseline，新机器使用不同依赖版本可能需要兼容调整。Python 3 用于测试；只有重新生成图标时需要 Pillow，正常编译使用已提交的图标文件。

## 2. 编译和测试

在仓库根目录的 PowerShell 执行：

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Configuration Debug
.\scripts\test.ps1 -Configuration Release
.\scripts\test.ps1 -Configuration Debug
```

构建会同时生成宿主、截图组件、StrikeSense 和独立核心测试。Release 用于发布；Debug 用于开发。输出在 `x64` 与 `artifacts`，这些目录不入库。

仅在开发机运行时，先正常关闭遥汐及 StrikeSense，然后执行：

```powershell
.\scripts\deploy.ps1 -Configuration Release
```

该命令注册本机开发布局，再从开始菜单打开遥汐。移动或删除此布局会影响本机应用。它不是发给其他用户的安装流程。

## 3. 生成安装包

```powershell
.\scripts\package.ps1
```

脚本构建 Release x64，并在 `artifacts\packages` 生成 MSIX、依赖目录和安装脚本。默认关闭签名，用于检查包内容；未签名包不能直接当成可安装发行版。

已有可用签名证书且私钥可访问时，将实际证书 SHA-1 指纹传给脚本：

```powershell
.\scripts\package.ps1 -CertificateThumbprint '替换为实际的40位证书指纹'
```

不要把占位文字直接作为证书指纹执行。脚本不创建、导出或上传证书。

证书 Subject 必须匹配 `Package.appxmanifest` 的 Publisher，当前是 `CN=jingy`。对外发布使用目标设备信任的签名证书，或走 Microsoft Store 发布流程；内部测试可以使用自签证书，但目标设备须由测试者自行信任对应证书。不要分发私钥或 PFX。签名与信任要求见 [Microsoft MSIX 签名说明](https://learn.microsoft.com/en-us/windows/msix/package/signing-package-overview)。

也可以在 Visual Studio 的“打包和发布 / 创建应用程序包”向导中选择旁加载（Sideloading）、Release、x64，配置证书后生成。具体流程见 [Microsoft 打包文档](https://learn.microsoft.com/en-us/windows/msix/package/packaging-uwp-apps)。

## 4. 给其他人安装

- 保留输出中的 `Dependencies`、安装脚本及其资源目录，连同已签名的 MSIX 一起分发。`.appxsym` 为调试符号，可自行留存。
- 用户可双击已签名的 MSIX；缺少依赖时，使用包输出中的安装脚本，或先安装 `Dependencies\x64` 中所需的框架包，再安装主包。
- 当前生成的清单依赖 `Microsoft.WindowsAppRuntime.2`（最低 2.5.1.0）和 `Microsoft.VCLibs.140.00.UWPDesktop`（最低 14.0.33728.0）。以本次包内清单为准，升级 SDK 后应重新检查。详见 [Windows App SDK 部署文档](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/deploy-overview)。
- 每次发布升级前，递增 `Package.appxmanifest` 中四段版本号；同一应用保持 Identity Name 与 Publisher 稳定。使用不同发布者身份时，应明确作为另一个应用分发，不能假定沿用原包的设置和升级关系。
- 正式发布前在干净的 Windows x64 设备或虚拟机上验证安装、启动、升级、卸载、运行库依赖及工具进程。当前工作只验证本机开发部署与未签名打包，没有验证异机安装或证书签名。

## 5. 修改后二次分发

修改源码后重复构建、测试、版本递增、签名和安装验证，并保留第三方来源及许可证。

当前仓库的实际授权边界：

- `Tools\ScreenShotPlus\LICENSE` 为 MIT；分发时保留版权与许可证，依赖声明见同目录 `THIRD_PARTY_NOTICES.md`。
- StrikeSense 依赖许可证位于 `Tools\Licenses\StrikeSense`，会随包复制。`Tools\StrikeSense\ORIGIN.md` 记录：原项目声明 MIT，但未随附项目 LICENSE；对外再分发前需补齐原作者授权依据。
- 主项目根目录尚无 LICENSE，不能仅凭当前仓库推定已向公众授予修改和再分发权。若你是主项目著作权人，可按你的发布意图补充许可证。
- 程序图标来自用户指定图片，来源见 `Assets\ORIGIN.md`；来源记录本身不授予图片公开再分发权，公开发行需具备相应授权。

以上说明记录仓库现有材料，不代替缺失的授权文件。打包成功与取得公开再分发授权是两项独立检查。
