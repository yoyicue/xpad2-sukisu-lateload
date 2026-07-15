# XPad2 SukiSU Ultra late-load 移植记录

日期：2026-07-15 至 2026-07-16
目标：TALIH PD2 `/260`，Android 13，Linux `4.19.191+`

## 基线

- 上游：`SukiSU-Ultra/SukiSU-Ultra`
- tag：`v4.1.3`
- commit：`0ca744a88835144c58d8256ebb32c279edabfcde`
- 官方 Manager：`v4.1.3 (40796)`
- 参考实现：本机 `xpad2-ksu-lateload` 的已验证 Linux 4.19 约束

SukiSU 与 KernelSU 已从 2025 年的共同基线长期分叉，因此采用按子系统审计后的语义移植，
没有直接 cherry-pick 旧项目补丁。

## 实现摘要

1. 用 `CONFIG_KSU_LEGACY_4_19=y` 隔离现代内核依赖，关闭目标未验证的 KPM、SUSFS、
   file wrapper、现代 mount namespace、seccomp cache 等路径。
2. 从 `el0_svc` 指令动态解析 syscall table；用 TTBR1 页表遍历定位物理页，并通过
   fixmap 写入和 readback 验证挂钩结果。
3. 对 reboot、setresuid、execve、newfstatat、faccessat 使用 4.19 直接 syscall
   adapter，保存原函数并提供退出 gate/active-call drain。
4. 适配 Linux 4.19 fsnotify、task work、nofault helper、flex_array policydb、旧
   SELinux state 和 AVC reset 接口。
5. 复制并交换当前 4.19 SELinux policydb，建立标准 `ksu`/`ksu_file` 域；不猜测
   隐藏 KASLR 数据地址。
6. loader 校验官方 Manager base APK 的包名和生产证书，从 `/data/data` 推导 appId，
   以只读 module parameter 在 zygote 启动 Manager 前固定身份。
7. 仅对已认证 Manager 的精确 `libksud.so debug su` exec 安装 non-CLOEXEC driver
   fd，解决 Android app seccomp 阻断 fallback syscall 的问题。
8. ksud 增加可复现版本覆盖和 `debug info`，明确输出 LKM/late-load/runtime mode 及
   Manager appId。

## 静态门禁

最终 `.ko`：

```text
SHA-256: 5835dbed566e9711fab02c3b729e6dce495b996481af53c474f2be4816e7fd81
srcversion: 93DD648468EDC27A2E4CCA6
vermagic: 4.19.191 SMP preempt mod_unload modversions aarch64
runtime imports: 141
missing from target runtime kallsyms: 0
```

最终 ksud：

```text
SHA-256: 74379a3c1a556448762db00d8e1316b31a4cf56a1eb1b8accd8447a1e3859bd8
version name: 4.1.3-xpad2
version code: 40796
embedded module source SHA-256: 5835dbed566e9711fab02c3b729e6dce495b996481af53c474f2be4816e7fd81
```

以下门禁全部通过：

- kernel module build in existing `mtk-kernel-x86` Lima；
- exact vermagic；
- runtime symbol gate；
- `cargo fmt --all -- --check`；
- `cargo ndk -t arm64-v8a check -p ksud`；
- clippy with `-D warnings`；
- release build；
- release build 已在最终模块更新后重跑。

## 首轮真机验证

初始 boot ID：`742ca893-5fd9-4358-8771-56b4f3eef21a`。开始时 `/proc/modules`
没有 KernelSU/SukiSU，SELinux Enforcing。官方 Manager 通过 xpad2 的 UID 10072
安装通道安装，PackageManager 结果为：

```text
package=com.sukisu.ultra
versionCode=40796
installer=com.tal.pad.znxxservice
appId=10221
certificate=947ae944f3de4ed4c21a7e4f7953ecf351bfa2b36239da37a34111ad29993eef
```

IonStack 临时 Root 在第 1/6 个 holder 成功，三个 capture worker 全部命中且原 fops
恢复。late-load 后：

```text
version: 40796
flags: 0x5
features: 0x5
lkm: true
late_load: true
runtime_mode: late-load
manager_appid: 10221
```

`ksud debug su` 返回：

```text
uid=0(root) gid=0(root) groups=0(root) context=u:r:ksu:s0
```

Manager 的 `libksud.so debug su` 也成功。随后执行 `xpad2 cleanup`，临时 Root socket、
client 和进程均消失，SELinux 恢复 Enforcing；SukiSU 模块、Root shell 和 Manager
仍工作，Boot ID 未改变。Manager 首页显示“工作中 [越狱模式]”、Manager/驱动版本
`40796`。

首轮 UI 审计发现 hook type 仍沿用上游固定字符串、legacy reboot adapter 被误记为
kprobe 失败。随后源码将其修正为 legacy direct syscall 标签和正常 info 日志。

## 第二轮真机验证

Boot ID：`63a4bd76-54a5-465b-8de6-b55d3935d359`。IonStack 临时 Root 在第
1/6 个 holder 成功，两个 capture worker 明确成功，原 fops 恢复。加载后驱动版本
`40796`、flags/features `0x5`、LKM/late-load 均为 true，Manager appId 为 `10221`；
`debug su` 返回 `uid=0`、`u:r:ksu:s0`。清理临时 Root 后 SELinux Enforcing，SukiSU
和 Manager 继续工作，Boot ID 未改变。

该轮验证的模块 SHA-256 为
`f020a2a94670a8bc91b024695b3d7e3ba9c9cf57ceae97070f7bd25c8d2e3140`。之后唯一源码改动是
把超过 31 字节的展示标签 `Direct Syscall Table (Legacy 4.19)` 缩短为
`Direct Syscall Table (4.19)`，以适配 32 字节 UAPI 字段；功能路径未改变。发布模块
重新构建并通过 141/141 运行时符号、精确 vermagic、fmt、check、clippy 和 release
build 门禁，没有在同一 boot 热替换。

## 生命周期约束

开发历史不作为在线卸载授权。当前产品策略固定为：同一 boot 不卸载、不替换；任何升级
或异常都先普通重启。KPM/SUSFS 不属于本目标的已验证能力。
