# 录屏「性能优化（Beta）」实现说明

本文说明 AeluraTool 实际使用的采集、缩放和编码路径。Beta 表示尝试使用更直接的显卡处理链路，不表示已经对所有显卡测得最高性能。

## 关闭 Beta 也能使用 GPU

「CPU / GPU」决定视频编码器。默认 DX11 的单显示器捕获现在统一使用可恢复的 GPU 桌面采集与图像处理，以支持独占全屏切换；「性能优化（Beta）」进一步尝试厂商直接编码接口，两者不是同一个开关。

| 选择 | 桌面区域采集与处理 | 视频编码 |
| --- | --- | --- |
| CPU | DXGI Desktop Duplication / D3D11 处理，读回 NV12 | Media Foundation 软件编码器 |
| GPU，Beta 关闭 | DXGI Desktop Duplication / D3D11 处理，读回 NV12 | Media Foundation 选择驱动提供的硬件编码器 |
| GPU，Beta 开启且初始化成功 | DXGI Desktop Duplication、D3D11 缩放与指针合成、视频处理器转换 NV12 | 按显卡厂商直接调用 NVENC 或 AMF |
| GPU，Beta 开启，优先 DX12 | DX11 兼容采集/裁剪/指针，GPU 共享纹理与 fence，DX12 缩放及 NV12 转换 | NVENC 原生 D3D12 接口，当前仅 NVIDIA |

普通 GPU 模式会检查实际选中的 Media Foundation 编码器是否为硬件编码器，不能满足时报告错误，不静默改为 CPU。

不支持默认采集管线的区域（例如跨显示器选区）仍可退回 GDI/WIC；该兼容路径不能保证捕获独占全屏。CPU 选项只指定编码位置，不意味着整个捕获过程不使用 GPU。

## 独占全屏与显示模式切换

`Recording.Desktop.*` 按显示器共享 Desktop Duplication 与 D3D11 设备。同一进程不能对同一输出重复创建 duplication，因此自由录制和重放共享取帧，各自保留独立裁剪/缩放输出、编码器、缓存与导出。设备即时上下文启用多线程保护，图像处理和读回还按源串行提交，防止不同任务的图形状态交错。

进入或退出独占全屏、桌面切换及显示模式变化导致 `DXGI_ERROR_ACCESS_LOST` 时，释放失效接口，以 250 ms 间隔重新枚举相同显示器并重建采集。暂时无权限或不可用时继续等待，允许停止；首帧超时不再当成失败。已取得画面后，恢复期间重复最后有效帧，保留音视频时间线与已有重放；界面显示正在恢复。恢复后的实际纹理尺寸重新检查，整屏源跟随新分辨率并缩放到初始编码画布，区域源保持相对显示器的位置。实际设备移除等非暂时错误仍会报告失败。

捕获端选择 DX11 不要求游戏使用 DX11，Vulkan/DX12 游戏的显示器输出也走同一捕获链路；这不是进程注入或游戏内部 Present hook，不保证绕过保护内容或其他捕获限制。DX12 模式也复用该捕获链路，支持全屏切换；界面明确标为「优先 DX12」，不再声称整条管线只有 DX12。

## Beta 具体减少了哪些工作

桌面采集通过 `IDXGIOutput1::DuplicateOutput` / `IDXGIOutputDuplication::AcquireNextFrame` 获取显卡纹理。根据采集显示器所属 DXGI 适配器的 Vendor ID 选择 NVIDIA（`0x10DE`）或 AMD（`0x1002`），不是简单检查机器上是否安装了某个品牌。

采集纹理进入 D3D11 着色器，完成裁剪、最近邻 / 双线性 / 双三次 / 面积近似缩放和鼠标指针合成。鼠标位图变化时上传新资源；常规画面不需要每帧读回完整桌面。

随后使用 `ID3D11VideoContext::VideoProcessorBlt` 将 BGRA 转成编码器需要的 NV12。输出尺寸向上对齐偶数，避免 NV12 对齐时丢掉源画面的末行或末列。

最终把 D3D11 NV12 纹理交给厂商编码器。编码后的 H.264 / H.265 数据包仍通过 Media Foundation 写入 MP4，系统声音与麦克风经 WASAPI 采集、混音及 AAC 编码；「直接调用显卡编码器」不意味着所有工作都在 GPU 上完成。

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

DX11 Beta 初始化不支持当前显卡、输出或编码能力时，尝试回退普通 GPU 路径并显示原因。「优先 DX12」在桥接或编码初始化不支持时自动回退 DX11，并显示实际状态。可恢复的全屏采集失效会重建；实际设备移除等不可恢复错误仍停止任务，磁盘缓存保留恢复信息。

跨显示器区域、旋转输出、HDR、受保护内容及独占全屏存在兼容限制；专有接口不能绕过 Windows 的捕获限制。更高的帧率、分辨率和码率仍增加 GPU、CPU、内存和磁盘负担。并行录制也可能碰到驱动硬件编码会话数量限制。

即时重放使用有界的压缩 MP4 分段缓存，保存时无损合并，不重新压缩整个片段；它持续占用编码资源，硬盘模式还持续写入，内存模式只在主动保存时写视频。保存成功会消费已保存片段，随后只积累新增画面。五秒分段精度意味着成片可能比设置时长短不到五秒。

本仓库只维护公开 SDK 头文件及许可证，不分发 NVIDIA / AMD 的驱动 DLL，也不依赖开发机的 SDK 绝对路径。固定来源和版本见 `Tools/ScreenRecorder/Vendor/*/SOURCE.txt`，许可见 `Tools/ScreenRecorder/THIRD_PARTY_NOTICES.md`。

## CUDA 更深层优化与 DX12 的区别

CUDA 可用于融合缩放、色彩转换、降噪、背景处理等计算，也可通过 CUDA 图形互操作连接 NVENC。要证明收益，应比较预热后的 GPU 耗时、端到端延迟、复制次数和游戏帧时间，而不是看到 CUDA 初始化成功就显示「已优化」。简单色彩转换已有显卡视频处理器，换成 CUDA 可能增加映射、同步和通用计算竞争；复杂降噪才更可能受益。当前未实现 CUDA 运算，不将方案描述成已启用功能。

DX12 是图形资源与命令提交接口，CUDA 是 NVIDIA 的计算接口，两者不是版本升级关系。保留 DX11 可覆盖现有 DXGI/WGC 互操作和 AMD/NVIDIA 编码路径。D3D11On12 仍使用 DX11 API，不能标为纯 DX12。

## 为什么旧纯 DX12 捕获受限，现如何保留 DX12

DX12 本身能够处理图像和编码。旧实现的问题在取帧：为了避免任何 DX11 调用，它使用 GDI BitBlt 获取桌面后上传 GPU；GDI 不是可靠的独占全屏游戏捕获通道。Windows Desktop Duplication 返回 D3D11 纹理，当前 Windows Graphics Capture 互操作也以 D3D11 帧池提供表面；不能通过把 DX11 设备简单改成 DX12 设备就完成迁移。这个限制不代表 DX12 无法录屏，也不要求 Vulkan/DX12 游戏改用 DX11。

按本次明确的兼容优先要求，界面改为「优先 Direct3D 12（实验）」：

1. 通过共享的 DXGI 捕获或 WGC 获取 D3D11 纹理，沿用全屏失效重建与窗口跟踪。D3D11 完成区域裁剪、必要的分辨率归一化与指针合成，输出 BGRA。
2. 在相同适配器上创建原生 ID3D12Device。共享 BGRA 纹理使用 NT 共享句柄，D3D11 CopyResource 直接在 GPU 上复制；D3D11 Context4 Signal 与 D3D12 Queue Wait 使用共享 fence 交接所有权。
3. D3D12 计算着色器执行最终输出缩放与 BT.601 限幅 NV12 转换，两个平面复制到原生 NV12 纹理，状态屏障及完成 fence 明确管理资源访问。
4. Recording.Nvenc12.cpp 继续直接注册原生 D3D12 NV12 输入及码流缓冲，通过 NVENC D3D12 输入/输出包装与 fence 编码 H.264/H.265，然后用 Media Foundation 封装视频和 AAC 音轨。

实时路径不再用 GDI 读取桌面，也不把每帧 BGRA 从 GPU 读回 CPU 再上传，不创建 D3D11On12 包装。不过它有实际的 GPU 纹理复制和同步，不能称为全链路零复制。保留的 CPU BGRA process 接口用于生成图案测试，不是实时取帧路径。

D3D12 与编码器包装对象保持到相应工作完成，创建/销毁继续串行管理，停止时结束编码会话、排空队列并释放共享句柄和资源。当前 NVENC D3D12 路径需要兼容 NVIDIA 设备；桥接或编码初始化不支持时退回原有 DX11 路径，并在界面明确显示。AMD 继续使用已实现的 DX11/AMF，而不是把未经实现的 AMD DX12 标成可用。

使用较新接口不自动意味着更快。新路径去掉了旧 GDI/CPU 帧上传，但仍需评估额外复制、同步、驱动及并发会话开销；没有游戏帧时间和功耗基准，不宣称优于 DX11 或达到显卡最大性能。参考 [Microsoft D3D12 互操作](https://learn.microsoft.com/en-us/windows/win32/direct3d12/direct3d-12-with-direct3d-11--direct-2d-and-gdi)、[NVIDIA DirectX 12 编码说明](https://developer.nvidia.com/blog/encoding-for-directx-12-with-video-codec-sdk-11-1/)。

## 系统声音与麦克风

旧代码只初始化了 eCapture 麦克风端点，因此即使录到画面也没有系统播放声音。现在新增默认开启、独立持久化的「系统声音」，旧配置缺少 systemAudio 时默认 true；麦克风沿用用户已有选择，两者都关闭时不创建音轨。

系统声音通过默认 eRender/eConsole 播放端点的 WASAPI shared-mode loopback 采集，不要求用户启用「立体声混音」或安装虚拟声卡。两个输入统一请求 48 kHz、双声道、16-bit PCM，由 Windows 音频引擎自动转换及重采样，后台线程每约 5 ms 拉取数据，避免低视频帧率时漏读音频包。各源有独立、有上限的队列，混音使用饱和相加，输出 AAC 与视频时间线对齐。暂停/等待首帧时丢弃待用音频，避免恢复后播放之前的声音。

默认设备变化时重新连接；设备不可用或麦克风权限拒绝时显示独立提示并每秒尝试重连，视频及另一有效音源继续，不静默显示为音频正常。只捕获 Windows 默认播放设备：应用指定到其他设备的声音、独占音频流或受保护音频不保证可录。没有实现回声消除，扬声器声音也被麦克风收进去时可能叠加。官方接口见 [WASAPI Loopback Recording](https://learn.microsoft.com/en-gb/windows/win32/coreaudio/loopback-recording)。

## 指定窗口与即时重放

指定窗口使用 Windows Graphics Capture 的 `CreateForWindow` 与 FreeThreaded 帧池，绑定 HWND 和进程 ID，窗口被移动、遮挡或失去焦点后仍捕获该窗口。调整窗口大小后缩放到初始输出尺寸；最小化时重复最后有效帧，关闭窗口结束并封装视频。受保护画面、无法捕获的窗口及系统权限限制仍适用，不能保证「无论如何」都取得内容。

自由录制与重放各有独立采集/编码任务。重放开关保存到 recording.json，程序启动时静默恢复，登录自启动沿用托盘入口；程序退出时异步停止两路任务、完成保存、删除正常重放缓存，保留启用偏好。导出失败的片段与恢复清单保留。用户仍需让程序在后台运行，退出后不继续缓存。

## 源码与验证

- `Tools/ScreenRecorder/Recording.Audio.*`：系统回放、麦克风混音、设备重连与独立采集线程。
- `Tools/ScreenRecorder/Recording.Gpu.cpp`：适配器选择、桌面采集、GPU 图像处理、CUDA 探测。
- `Tools/ScreenRecorder/Recording.Nvenc.cpp`、`Recording.Amf.cpp`：厂商编码接口。
- `Tools/ScreenRecorder/Recording.Dx12.cpp`、`Recording.Nvenc12.cpp`：原生 DX12 图形处理与 NVENC。
- `Tools/ScreenRecorder/Recording.Window.cpp`：按窗口持续捕获。
- `Tools/ScreenRecorder/Recording.Media.cpp`：普通编码、音频及 MP4 写入。
- `Tools/ScreenRecorder/Recording.cpp`、`Recording.Remux.cpp`：任务与重放分段缓存、无损合并。
- `VERIFICATION.md`：实际完成的构建、测试和硬件验证边界。没有经过基准测试的路径不宣称「最大性能」或固定百分比提升。

Windows 官方接口说明：[Desktop Duplication](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api)、[窗口捕获 CreateForWindow](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow)。
