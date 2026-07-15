# XPad2 SukiSU Ultra late-load v0.1.0

首个面向学而思 XPad2/TALIH PD2 Android 13 `/260`、Linux `4.19.191+` 的
SukiSU Ultra late-load Release。基于官方 SukiSU Ultra v4.1.3，驱动和 Manager
版本码均为 40796；无需修改 boot image，普通重启后模块自然消失。

已在物理 XPad2 验证 late-load、官方 Manager 授权、`u:r:ksu:s0` Root、临时 Root
清理和 SELinux Enforcing。发布模块通过精确 vermagic 和 141/141 运行时符号门禁。

Release 包含：

- `sukisu-xpad2-4.19.191.ko`：独立内核模块；
- `ksud-sukisu-xpad2`：内嵌同一目标模块的 loader；
- `SHA256SUMS` 与 `RELEASE-README.txt`。

Manager APK 请从 [SukiSU Ultra 官方 v4.1.3 Release](https://github.com/SukiSU-Ultra/SukiSU-Ultra/releases/tag/v4.1.3)
下载，本 Release 不重复发布上游签名 APK。

仅支持 `/260`。KPM 和 SUSFS 在该 4.19 目标上不可用。同一 boot 不要卸载、替换或
重复加载模块；升级或异常时先普通重启。
