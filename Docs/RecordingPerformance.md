# 录屏「性能优化（Beta）」实现说明

本文说明 AeluraTool 实际使用的采集、缩放和编码路径。Beta 表示尝试使用更直接的显卡处理链路，不表示已经对所有显卡测得最高性能。

## 关闭 Beta 也能使用 GPU

「CPU / GPU」决定视频编码器。「性能优化（Beta）」决定是否进一步尝试 GPU 采集、GPU 图像处理及厂商直接编码接口，两者不是同一个开关。

| 选择 | 桌面区域采集与处理 | 视频编码 |
| --- | --- | --- |
| CPU | GDI 读取桌面，WIC 缩放，CPU 转换 NV12 | Media Foundation 软件编码器 |
| GPU，Beta 关闭 | GDI / WIC / CPU 图像处理 | Media Foundation 选择驱动提供的硬件编码器 |
| GPU，Beta 开启且初始化成功 | DXGI Desktop Duplication、D3D11 缩放与指针合成、视频处理器转换 NV12 | 按显卡厂商直接调用 NVENC 或 AMF |
| GPU，Beta 开启，原生 DX12 | GDI 读取桌面并上传 BGRA，D3D12 计算着色器缩放并转换 NV12 | NVENC 原生 D3D12 接口，当前仅 NVIDIA |

普通 GPU 模式会检查实际选中的 Media Foundation 编码器是否为硬件编码器，不能满足时报告错误，不静默改为 CPU。

## Beta 具体减少了哪些工作

桌面采集通过 `IDXGIOutput1::DuplicateOutput` / `IDXGIOutputDuplication::AcquireNextFrame` 获取显卡纹理。根据采集显示器所属 DXGI 适配器的 Vendor ID 选择 NVIDIA（`0x10DE`）或 AMD（`0x1002`），不是简单检查机器上是否安装了某个品牌。

采集纹理进入 D3D11 着色器，完成裁剪、最近邻 / 双线性 / 双三次 / 面积近似缩放和鼠标指针合成。鼠标位图变化时上传新资源；常规画面不需要每帧读回完整桌面。

随后使用 `ID3D11VideoContext::VideoProcessorBlt` 将 BGRA 转成编码器需要的 NV12。输出尺寸向上对齐偶数，避免 NV12 对齐时丢掉源画面的末行或末列。

最终把 D3D11 NV12 纹理交给厂商编码器。编码后的 H.264 / H.265 数据包仍通过 Media Foundation 写入 MP4，麦克风经 WASAPI 采集、AAC 编码；「直接调用显卡编码器」不意味着所有工作都在 GPU 上完成。

## NVIDIA：NVENC

实现位于 `Tools/ScreenRecorder/Recording.Nvenc.cpp`，使用公开的 `nvEncodeAPI.h` 声明，并在运行时从系统目录加载显卡驱动提供的 `nvEncodeAPI64.dll`。

通过 NVENC 函数表创建 D3D11 编码会话，注册 / 映射输入纹理，提交画面，取出编码码流并处理关键帧与编码头。当前使用 P1 速度预设、低延迟模式和 CBR，关闭 lookahead 和 AQ，避免为了更高压缩效率增加额外分析开销。缩放和色彩转换使用 D3D11，压缩主要由专用视频编码硬件承担，而非把 H.264 / H.265 软件算法搬到 CUDA 核心上运行。

## AMD：AMF

实现位于 `Tools/ScreenRecorder/Recording.Amf.cpp`，使用 AMD AMF 的公开头文件，运行时从系统目录加载驱动提供的 `amfrt64.dll`。

创建 AMF 工厂、上下文及 H.264 / H.265 编码组件，使用 D3D11 上下文和纹理表面，通过 `SubmitInput` / `QueryOutput` 提交和取得编码数据。当前使用速度预设、超低延迟和 CBR，H.264 关闭 B 帧重排。AMF 负责调用驱动支持的视频编码能力。

## CUDA 当前做到哪一步

代码只从系统目录加载 `nvcuda.dll`，调用 `cuInit` 和 `cuDeviceGetCount` 探测驱动是否可用。当前没有 CUDA 内核，也没有 NVRTC 编译、CUDA 缩放或额外的 CUDA 后处理。

显示「CUDA 驱动可用」只表示探测成功，不表示已启用 CUDA 运算。当前采用 D3D11 纹理互操作以减少中间复制；CUDA 是否更快需要针对具体操作实测，不能仅凭机器有 NVIDIA 显卡作出保证。

## 回退、性能成本及边界

DX11 Beta 初始化不支持当前显卡、输出或编码能力时，尝试回退普通 GPU 路径并显示原因。显式选择原生 DX12 后不进行 DX11 回退；不兼容时报告失败。录制过程中显卡设备丢失或采集失败会停止任务并保留恢复信息，不把设备错误伪装成成功录像。

跨显示器区域、旋转输出、HDR、受保护内容及独占全屏存在兼容限制；专有接口不能绕过 Windows 的捕获限制。更高的帧率、分辨率和码率仍增加 GPU、CPU、内存和磁盘负担。并行录制也可能碰到驱动硬件编码会话数量限制。

即时重放使用有界的压缩 MP4 分段缓存，保存时无损合并，不重新压缩整个片段；但会持续占用编码和磁盘写入资源。五秒分段精度意味着成片可能比设置时长短不到五秒。

本仓库只维护公开 SDK 头文件及许可证，不分发 NVIDIA / AMD 的驱动 DLL，也不依赖开发机的 SDK 绝对路径。固定来源和版本见 `Tools/ScreenRecorder/Vendor/*/SOURCE.txt`，许可见 `Tools/ScreenRecorder/THIRD_PARTY_NOTICES.md`。

## CUDA 更深层优化与 DX12 的区别

CUDA 可用于融合缩放、色彩转换、降噪、背景处理等计算，也可通过 CUDA 图形互操作连接 NVENC。要证明收益，应比较预热后的 GPU 耗时、端到端延迟、复制次数和游戏帧时间，而不是看到 CUDA 初始化成功就显示「已优化」。简单色彩转换已有显卡视频处理器，换成 CUDA 可能增加映射、同步和通用计算竞争；复杂降噪才更可能受益。当前未实现 CUDA 运算，不将方案描述成已启用功能。

DX12 是图形资源与命令提交接口，CUDA 是 NVIDIA 的计算接口，两者不是版本升级关系。保留 DX11 可覆盖现有 DXGI/WGC 互操作和 AMD/NVIDIA 编码路径。D3D11On12 仍使用 DX11 API，不能标为纯 DX12。

## 本次新增的原生 DX12

在录屏设置选择「原生 DX12」后，自动选用 GPU 与性能优化。`Recording.Dx12.cpp` 创建原生 `ID3D12Device`、命令队列、上传缓冲、BGRA/NV12 资源、描述符、计算管线、资源屏障与 fence。计算着色器执行缩放和 BT.601 限幅 NV12 转换，然后把两个平面复制进 NV12 纹理。

`Recording.Nvenc12.cpp` 以该 D3D12 设备打开 NVENC 会话，明确设置输入格式及最大尺寸，注册原生输入纹理和 READBACK 码流缓冲，通过 `NV_ENC_INPUT_RESOURCE_D3D12` / `NV_ENC_OUTPUT_RESOURCE_D3D12` 与 fence 同步。H.264、H.265 使用同样的 P1/低延迟/CBR 策略；编码后的码流封装仍使用 Media Foundation。

D3D12 包装对象保持到编码器销毁，停止时结束包括空片段在内的编码会话并排空图形队列。设备与编码器的创建/销毁串行处理，避免并行任务同时拆卸驱动资源；逐帧采集和处理仍由各自任务执行，忙碌状态持续到资源释放结束。

这条录制后端不创建 D3D11 设备、不调用 D3D11On12、不进行 DX11 回退。这里的「纯 DX12」指图形处理与 NVENC 互操作后端；WinUI 界面、系统合成器或视频播放器内部使用什么图形接口，不受录制后端控制。

**采集边界必须单独说明：当前用 GDI 获取区域或整屏，再上传 D3D12，尚不是 GPU 零拷贝采集。** Desktop Duplication 和当前窗口捕获实现依赖 DX11 互操作，不能把它们包在 D3D11On12 上称为纯 DX12。因此原生 DX12 当前仅支持 NVIDIA 的区域录制与整屏重放；按窗口持续捕获需选择 DX11，AMD 使用原有 DX11/AMF。没有兼容 NVIDIA 设备或驱动不支持时直接报告错误。

原生 DX12 是实际可运行的可选后端，不表示比默认 DX11 更快：GDI 采集、每帧上传及同步仍有成本。默认保留 DX11；尚未进行游戏帧时间、功耗或同参数吞吐基准测试。NVIDIA 也说明不同 DirectX 接口的编码质量相同、编码性能接近，不能仅依据接口版本推断收益。参考 [NVIDIA DirectX 12 编码说明](https://developer.nvidia.com/blog/encoding-for-directx-12-with-video-codec-sdk-11-1/) 与 [NVENC API 编程指南](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html)。

## 指定窗口与即时重放

指定窗口使用 Windows Graphics Capture 的 `CreateForWindow` 与 FreeThreaded 帧池，绑定 HWND 和进程 ID，窗口被移动、遮挡或失去焦点后仍捕获该窗口。调整窗口大小后缩放到初始输出尺寸；最小化时重复最后有效帧，关闭窗口结束并封装视频。受保护画面、无法捕获的窗口及系统权限限制仍适用，不能保证「无论如何」都取得内容。

自由录制与重放各有独立采集/编码任务。重放开关保存到 recording.json，程序启动时静默恢复，登录自启动沿用托盘入口；程序退出时异步停止两路任务、完成保存、删除正常重放缓存，保留启用偏好。导出失败的片段与恢复清单保留。用户仍需让程序在后台运行，退出后不继续缓存。

## 源码与验证

- `Tools/ScreenRecorder/Recording.Gpu.cpp`：适配器选择、桌面采集、GPU 图像处理、CUDA 探测。
- `Tools/ScreenRecorder/Recording.Nvenc.cpp`、`Recording.Amf.cpp`：厂商编码接口。
- `Tools/ScreenRecorder/Recording.Dx12.cpp`、`Recording.Nvenc12.cpp`：原生 DX12 图形处理与 NVENC。
- `Tools/ScreenRecorder/Recording.Window.cpp`：按窗口持续捕获。
- `Tools/ScreenRecorder/Recording.Media.cpp`：普通编码、音频及 MP4 写入。
- `Tools/ScreenRecorder/Recording.cpp`、`Recording.Remux.cpp`：任务与重放分段缓存、无损合并。
- `VERIFICATION.md`：实际完成的构建、测试和硬件验证边界。没有经过基准测试的路径不宣称「最大性能」或固定百分比提升。

Windows 官方接口说明：[Desktop Duplication](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api)、[窗口捕获 CreateForWindow](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow)。
