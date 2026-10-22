# ROCCAT VirtualHID 可行性核查

2026-10-22：用户授权只读逆向，并明确授权安装 Swarm 自带 VirtualHID 及验证。

结论：厂商签名虚拟 HID 驱动已安装运行，实际无按键报告回环通过，具备作为 AutoClicker 后端的基础。现有工具后端未切换，尚未验证真实键鼠输出、同键触发或游戏兼容性。

## 证据

- 物理鼠标 VID_1E7D/PID_2DD2，包含键盘、鼠标及厂商集合。物理键盘接口服务仍为 kbdhid，INF 为 keyboard.inf，没有替换物理鼠标驱动。
- Swarm 的 data/2DD2/2DD2.dll 与 HIDDLL.dll 是 x86 DLL，主要提供 HID 读写接口，不能直接装入 x64 工具箱。Monitor 中找到 Kone Pure Ultra 专用宏读写及按住执行字符串，同时存在 keybd_event 导入；不能据此认定所有宏均在硬件或内核执行。
- virtual_hid/x64/VirtualHID.sys 是 KMDF x64 驱动，INF 版本 1.0.0.2、日期 2019-12-24，硬件 ID 为 TurtleBeach/VirtualHID（INF 中使用反斜杠）。SYS 签名 Valid，签名者 Voyetra Turtle Beach；CAT 签名 Valid，签名者 Microsoft Windows Hardware Compatibility Publisher。
- 随附 devcon.exe 未签名，本次没有执行。通过 Windows SetupAPI/NewDev API 安装原始签名驱动，没有修改 SYS/INF/CAT，没有处理固件文件或绕过签名验证。
- 静态反汇编确认有键盘、鼠标和厂商 HID 描述符，以及将厂商输出报告负载复制给待完成 HID 读请求的路径。没有利用或修改内核代码，也未向未知 IOCTL 写入。

## 实际安装与测试

- 新根设备 ROOT/VIRTUALHID/0000（实际实例路径使用反斜杠），驱动存储 oem15.inf，服务 VirtualHID Running，设备问题码 0，子集合均为 OK。
- 安装器报告 reboot=True，未重启系统；目前通信已经通过，重启后的状态未验证。
- 安装 SYS 与 Swarm 原文件 SHA-256 相同：6BD8F1E1B56A900DA171F3E27EA372C62B6F74DADDDBB77A31F82416B0403CC5。
- 只打开路径含 VirtualHID 的新厂商集合。发送 65 字节输出，读回 33 字节厂商输入，标记 AELURA-LOOPBACK-TEST 逐字节相同。使用有界 overlapped I/O 并清理句柄。
- 未发送键盘或鼠标报告，没有向游戏发送点击，没有改宏、DPI、灯光、按键分配或固件。
- 证据与研究探针保留在忽略目录 artifacts：roccat-driver-install.log、roccat-driver-loopback.json、roccat-driver-probe.log、roccat-virtualhid-disassembly.txt、install-roccat-virtualhid.ps1、probe-roccat-virtualhid.py。没有将厂商二进制加入仓库或发布包。

## 接入限制

仍需实现驱动检测、Windows 按键到 HID Usage 的转换、配对按下/松开和失败清理，并验证设备移除、取消和权限问题。现有引擎用 LLMHF_INJECTED 排除自身输出，不能假设对虚拟 HID 仍有效；需要按原始输入设备来源等机制避免同键反馈。仅有回环不能替代输入验收。

该方案属于虚拟 HID 输入，不等于鼠标板载宏或物理点击，不保证游戏兼容性。板载宏协议尚未验证。第三方驱动再分发授权和其他机器兼容性未确认，暂不作为产品默认依赖。

## 恢复信息

驱动保留在本机，未自动卸载。如需撤销，可在管理员终端使用 Windows 自带 pnputil /remove-device，参数仅指定本次 ROOT/VIRTUALHID/0000 实例（将分隔符换为反斜杠）。不要删除物理 HID 或鼠标设备。驱动存储 oem15.inf 可先保留；删除前须确认没有其他设备依赖。本次未执行卸载或重启。

## 公开参考

- [官方说明：板载内存与规格](https://cdn.turtlebeach.com/device/qig/kone-pure-ultra/kone-pure-ultra_209-8640-v1.pdf)
- [Eruption 的设备实现](https://github.com/eruption-project/eruption/blob/master/eruption/src/hwdevices/roccat_kone_pure_ultra.rs)：仅作 HID 配置参考，没有复制其 GPL 源码，也不将 RGB 支持视为宏支持。
