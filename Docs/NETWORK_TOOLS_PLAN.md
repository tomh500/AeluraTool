# 网络工具扩展方案（2026-10-25）

通过用户已打开的 Edge ChatGPT 对话讨论后确定三个独立入口：网络测速、DNS 查询、TCP 端口检测。讨论链接：https://chatgpt.com/c/6addf45d-ff74-83ee-a523-6757b11bbfff

## 实现范围

- 网络分类和全局搜索注册稳定工具 ID `netspeed`、`dnslookup`、`tcpcheck`。各自保留输入与结果，支持系统/浅/深主题和即时语言切换。
- 三个工具在统一快捷键管理中提供默认关闭的绑定，可搜索、编辑和启停，失败沿用原有回滚机制。
- 网络核心与 WinUI 面板分离。请求手动触发，后台执行，有截止时间、取消入口及可复制结果；不修改系统网络设置。
- DNS：`DnsQueryEx` 查询 A/AAAA 和 TTL，区分无记录、查询错误、超时、取消；系统缓存可能参与查询。IP 字面量直接展示，TTL 为 0。
- TCP：按地址族解析，使用非阻塞 Winsock，轮询连接并检查 `SO_ERROR`。单端口测试，不发送应用数据；显示实际地址、握手时间与拒绝/不可达/超时状态。
- 测速：WinHTTP 直连固定 HTTPS 测速端点。指定地址族的 DNS 结果通过 `WINHTTP_OPTION_RESOLUTION_HOSTNAME` 固定连接地址，原域名保留用于 Host、SNI 和证书验证；不降低证书安全设置。响应后核验 `WINHTTP_OPTION_CONNECTION_INFO`，拒绝错误协议及 IPv4 映射 IPv6。
- 测速禁止重定向和自动身份验证，使用禁缓存参数和 identity 编码。上传随机内存数据；只有请求完整上传且服务器返回成功，才报告速度。下载须完整收到指定字节数。
- 采用固定数据量单连接测量：每方向 1/10/50 MB。两种协议均测试时约消耗 4/40/200 MB，加协议开销。测速值以 HTTP 事务总时长计算（不含 DNS），计入 TCP/TLS、服务器响应和上传确认；短样本标明局限，避免称为带宽上限。
- 分别运行 IPv4 下载、上传、IPv6 下载、上传。一种协议失败不伪装成另一协议成功；取消后不再开始下一项。WinHTTP 使用原生异步请求，后台工作线程以完成事件等待；取消或整体截止时间到达后，在没有 API 调用正在执行时关闭异步请求，等待最终 HANDLE_CLOSING 回调后释放上下文和传输缓冲。连接/发送/接收的原生超时另设 3 秒。

## 验证方式

核心回归在本机临时回环端口提供 IPv4-only 与 IPv6-only HTTP 服务，核对实际下载字节和服务器收到的上传字节。涵盖跨协议拒绝、端口关闭、HTTP 302/500、下载不完整、取消和截止时间；不以公共网络可用性决定自动测试成败。

`CoreTests.exe --network-live` 是显式手动公网冒烟入口，分别向 speed.cloudflare.com 上传/下载 1 MB，并输出每项状态、地址、错误码、字节、耗时和 Mbps。退出码只表示探测执行完毕，不能作为四项联网成功的断言。默认测试不使用公网流量。

交付验证结果、部署与环境局限另记入 `VERIFICATION.md`。

## 参考

- Microsoft WinHTTP options: https://learn.microsoft.com/en-us/windows/win32/winhttp/option-flags
- Cloudflare speed test service: https://speed.cloudflare.com/

该版本不含多连接带宽饱和测试、自定义服务器、延迟抖动统计或长时间历史存储。
