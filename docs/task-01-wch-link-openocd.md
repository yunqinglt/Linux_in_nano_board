# 任务 1：WCH-Link / OpenOCD 调试入口确认

记录时间：2026-09-21（Asia/Shanghai）

## 结论

当前机器已经具备后续调试所需的官方 WCH OpenOCD，不需要先安装第三方二进制：

- OpenOCD：`E:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin\openocd.exe`
- 版本：`Open On-Chip Debugger 0.11.0+dev-snapshot (2026-08-25-16:45)`
- SHA-256：`0F49153F490E8D89B391DA9717E01F67C380F641C59600AEC483113762469B65`
- 配置：同目录的 `wch-riscv.cfg`
- 已静态验证：OpenOCD 的 `adapter list` 包含 `wlinke`；配置使用 `adapter driver wlinke`、`transport select sdi` 和 `target ... wch_riscv`。

这里的两线 RISC-V 调试传输在 WCH OpenOCD 中名为 **SDI**。板上引脚可以标作 `SWDIO/SWCLK`，但不能用普通 ARM SWD OpenOCD 配置替代 `wlinke + sdi + wch_riscv`。

不要直接调用 PATH 中的 `openocd`。本机 PATH 当前解析到 `E:\PuyaSDE\openocd\bin\openocd.exe` 0.12.0，该构建的 adapter 列表没有 `wlinke`。

## 当前 USB 实测

| 角色 | VID:PID | Windows 观测 | 结论 |
| --- | --- | --- | --- |
| WCH-Link 调试接口 | `1A86:8010`, `MI_00` | `WCH-LinkRV`，驱动服务 `WCHLink_A64`，驱动版本 `1.2.2022.8` | 已正常枚举 |
| WCH-Link 串口接口 | `1A86:8010`, `MI_01` | `WCH-Link SERIAL (COM19)`，服务 `usbser`，驱动版本 `1.6.2023.5` | 已正常枚举 |
| CH32V203 USB Bootloader | `4348:55E0` | `USB Module`，服务 `CH375_A64`，驱动版本 `3.5.2025.8` | 已正常枚举，目标芯片当前可由其自身 USB Bootloader 感知 |

`1A86:8010` 与 `4348:55E0` 是同一 USB Hub 上两个独立的设备/容器，后者不是 WCH-Link 的子接口。因此：

- `4348:55E0` 证明目标的 USB Bootloader 当前存在；
- `1A86:8010` 证明 WCH-Link 调试器和串口当前存在；
- 以上两点不能单独证明 PA13/PA14 上的 SDI 物理链路已经成功，也不能证明 OpenOCD 已读出芯片 ID。

## 无侵入复核

仓库脚本只枚举当前 PnP 设备并检查 OpenOCD 是否编入 `wlinke`。它不会执行 OpenOCD `init`，不会打开串口，也不会 halt、reset 或写 Flash：

```powershell
.\tools\Test-WchDebugPrerequisites.ps1 -Require Both
.\tools\Test-WchDebugPrerequisites.ps1 -Require DebugPrerequisites -Json
```

退出码为 `0` 表示所选要求满足；设备或调试前置条件不满足时为 `2`。

本轮没有运行以下目标连接命令：

```powershell
& 'E:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin\openocd.exe' `
  -f 'E:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin\wch-riscv.cfg'
```

原因是 WCH RISC-V OpenOCD 的 `examine()` 会重置 Debug Module，并在目标原先运行时临时 halt 后再 resume。目标当前正处于 USB Bootloader 模式，仅为了“看见设备”不应扰动其运行状态。后续若需要证明 SDI 链路和芯片身份，应把这一步作为显式的目标连接测试来执行，并单独记录是否保持/恢复 Bootloader 状态。

## GitHub 移植调查

已确认以下代码库确实含 WCH-Link OpenOCD 支持，而不是只在 README 中提到它：

1. [Seneral/riscv-openocd-wch](https://github.com/Seneral/riscv-openocd-wch)，默认分支 `wch-linke-sdi`，调查时 HEAD 为 `bde507c1e98514162bdbe4275662027a46b98e55`。`src/jtag/drivers/wlinke.c` 明确列出 VID `0x1a86`、PID `0x8010`，同时包含 WCH RISC-V、SDI 和 `CH32V20X` 支持。它适合作为可审计源码参考或后备自建来源。
2. [fxsheep/openocd_wchlink-rv](https://github.com/fxsheep/openocd_wchlink-rv)，源码同样匹配 `0x1a86:0x8010`，但最后调查到的提交是 2022 年，早于当前 MounRiver 构建。
3. [xu7wong/openocd_wchlink](https://github.com/xu7wong/openocd_wchlink)，README 针对 WCH-Link 固件 2.4/2.5，最后调查到的提交也是 2022 年。
4. [Community-PIO-CH32V/tool-openocd-riscv-wch](https://github.com/Community-PIO-CH32V/tool-openocd-riscv-wch) 提供打包分发，但其 README 要求源码/构建细节向 MounRiver 支持渠道索取。

本项目首先固定使用已安装、版本更新且与官方 IDE 启动配置一致的 MounRiver OpenOCD。是否把某个源码移植作为 submodule 属于任务 2 的仓库整理范围，本次不提前引入。

## 本任务的验证边界

- 已确认：两个 USB VID:PID 当前都在场、驱动正常、WCH-Link 串口为 COM19、官方 OpenOCD 含正确 adapter/transport/target 支持。
- 未执行：OpenOCD `init`、SDI 芯片 ID 读取、halt/reset、下载、Flash 擦写、串口收发。
- 因此本任务证明“设备与可用调试器存在”，不声称“目标调试链路已连接成功”。
