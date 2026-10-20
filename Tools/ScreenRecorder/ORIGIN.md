# 屏幕录制

项目自身的 Media Foundation / WASAPI 录制模块，无外部录制进程或下载依赖。

录屏通过进程内固定通信协议直接复用本仓库 `Tools/ScreenShotPlus/capture.cpp` 的
区域选择模式（MIT，版权和许可见 `../ScreenShotPlus/LICENSE`），不写入截图或更改剪贴板。
设置、录制控制、保存提示和预览界面由 WinUI 3 构建；未修改或回写独立 ScreenShotPlus 原项目。

Windows Media Foundation 编码器由操作系统和显卡驱动提供；H.265 及硬件编码可用性取决于本机安装的编码器。

性能优化 Beta 使用 DXGI Desktop Duplication、D3D11 着色器缩放/指针合成、视频处理器 NV12 转换，
按采集输出所属适配器 Vendor ID 调用 NVIDIA NVENC 或 AMD AMF。CUDA 仅探测驱动可用性，
当前像素链路为 D3D11 互操作，不声称运行 CUDA 内核。跨显示器、旋转输出及不支持的驱动在初始化时回退标准 GPU 路径。
Vendor 内只保留公开 SDK 头文件及固定 commit 的来源信息，许可见 THIRD_PARTY_NOTICES.md 与各文件原始声明。
