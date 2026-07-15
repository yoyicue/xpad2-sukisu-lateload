# XPad2 SukiSU Ultra late-load

这是面向学而思 XPad2/TALIH PD2 `/260` 固件的 SukiSU Ultra late-load
移植。它在取得一次临时 Root 后动态加载 SukiSU，不修改 boot image；普通重启后
模块自然消失。

仓库以官方 SukiSU Ultra `v4.1.3`、提交
`0ca744a88835144c58d8256ebb32c279edabfcde` 为可复现基线。Linux 4.19 兼容层来自
对 `xpad2-ksu-lateload` 已验证约束的语义移植，不是把两个已经分叉的项目直接合并。

## 唯一支持的目标

- 产品/设备：`TALIH_PD2` / `ls12_mt8797_wifi_64`
- 固件：Android 13，build `/260`
- 内核：`4.19.191+`，arm64，MediaTek MT8797
- vermagic：`4.19.191 SMP preempt mod_unload modversions aarch64`
- OEM 编译器：Android clang `r383902`
- late-load KMI：`xpad2-sukisu-4.19.191`
- SukiSU 驱动/Manager 版本码：`40796`

这是内核 ABI 和固件绑定的移植。同型号、不同系统版本也不能据此视为兼容；不要把
制品加载到 `/260` 之外的机器。

## 当前状态

已经在物理 XPad2 上验证：

- 从当前 boot 未加载任何 KernelSU/SukiSU 模块的状态进入；
- 官方签名 SukiSU Ultra Manager `v4.1.3 (40796)`；
- 驱动 `40796`，flags/features `0x5`，LKM + late-load；
- Manager appId 在加载前经 APK 证书校验，并以只读模块参数提前固定；
- `ksud debug su` 得到 `uid=0` 和 `u:r:ksu:s0`；
- Manager 的 `libksud.so debug su` 能跨 exec 获得 Root；
- 清除 IonStack 临时 Root 后，SukiSU 继续工作且 SELinux 为 Enforcing；
- Boot ID 在整个加载和验证过程中保持不变。

功能路径的第二个独立 boot 验证记录见 `PROGRESS.md`。真机验证完成后仅将 Manager
展示用 hook 标签缩短为 `Direct Syscall Table (4.19)`，功能代码未再改动；发布制品已
重新通过编译、ABI、符号和 Rust 门禁。

## 制品

```text
5835dbed566e9711fab02c3b729e6dce495b996481af53c474f2be4816e7fd81  artifacts/sukisu-xpad2-4.19.191.ko
74379a3c1a556448762db00d8e1316b31a4cf56a1eb1b8accd8447a1e3859bd8  artifacts/ksud-sukisu-xpad2
```

`ksud-sukisu-xpad2` 内嵌的模块名是
`xpad2-sukisu-4.19.191_kernelsu.ko`。Manager APK 不在本仓库和本项目 Release
重复发布，请从 [SukiSU Ultra 官方 v4.1.3 Release](https://github.com/SukiSU-Ultra/SukiSU-Ultra/releases/tag/v4.1.3)
下载生产签名制品；其 SHA-256 是
`1b1e837c0a5b6aa34554882fad67cef6db6ca1a84d43e07dd904cf54f8d261ae`。加载器同时校验其证书尺寸
`0x35c` 和 SHA-256
`947ae944f3de4ed4c21a7e4f7953ecf351bfa2b36239da37a34111ad29993eef`。

## 使用

先下载并用 `xpad2` 安装官方 Manager；该固件禁止普通 `adb install`：

```sh
curl -LO https://github.com/SukiSU-Ultra/SukiSU-Ultra/releases/download/v4.1.3/SukiSU_v4.1.3_40796-release.apk
adb push SukiSU_v4.1.3_40796-release.apk /data/local/tmp/
adb shell /data/local/tmp/xpad2 install apk \
  /data/local/tmp/SukiSU_v4.1.3_40796-release.apk
```

推送 loader，取得临时 Root 后加载：

```sh
adb push artifacts/ksud-sukisu-xpad2 /data/local/tmp/
adb shell chmod 755 /data/local/tmp/ksud-sukisu-xpad2
adb shell /data/local/tmp/xpad2 root
adb shell /data/local/tmp/su -c \
  '/data/local/tmp/ksud-sukisu-xpad2 late-load --kmi xpad2-sukisu-4.19.191'
```

不要传 `--allow-shell`、`--spoof-release` 或 `--spoof-version`；XPad2 legacy 目标不暴露
这些模块参数。加载完成后清理临时 Root：

```sh
adb shell /data/local/tmp/xpad2 cleanup
```

验证：

```sh
adb shell /data/local/tmp/ksud-sukisu-xpad2 debug info
adb shell 'printf "id\ncat /proc/self/attr/current\nexit\n" | \
  /data/local/tmp/ksud-sukisu-xpad2 debug su'
```

## 构建

内核模块必须在 x86_64 Linux 中使用匹配的 vendor kernel output 和 OEM clang。示例：

```sh
export PATH=/path/to/clang-r383902/bin:$PATH
make -C /path/to/TALIH-PD2-kernel-4.19.191 \
  O=/path/to/kernel-out \
  M="$PWD/kernel" \
  ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  CLANG_TRIPLE=aarch64-linux-gnu- LLVM_IAS=1 \
  CC=clang LD=ld.lld AR=llvm-ar NM=llvm-nm \
  OBJCOPY=llvm-objcopy OBJDUMP=llvm-objdump STRIP=llvm-strip \
  CONFIG_KSU=m CONFIG_KSU_LEGACY_4_19=y \
  KSU_VERSION=40796 \
  KSU_VERSION_FULL=v4.1.3-xpad2-lateload@main \
  KCFLAGS='-Wno-strict-prototypes -Wno-int-conversion -Wno-gcc-compat -Wno-missing-prototypes -Wno-declaration-after-statement -Wno-unused-function' \
  modules -j4
```

将 `.ko` 嵌入 ksud，再构建 userspace：

```sh
install -m 0644 kernel/kernelsu.ko \
  userspace/ksud/bin/aarch64/xpad2-sukisu-4.19.191_kernelsu.ko

KSU_VERSION_CODE=40796 KSU_VERSION_NAME=4.1.3-xpad2 \
  cargo ndk -t arm64-v8a check -p ksud
KSU_VERSION_CODE=40796 KSU_VERSION_NAME=4.1.3-xpad2 \
  cargo ndk -t arm64-v8a clippy -p ksud -- -D warnings
cargo fmt --all -- --check
KSU_VERSION_CODE=40796 KSU_VERSION_NAME=4.1.3-xpad2 \
  cargo ndk -t arm64-v8a build --release -p ksud
```

## 4.19 移植边界

legacy 路径实现了动态 `sys_call_table` 解码、TTBR1 页表遍历、fixmap 写入及 readback、
直接 syscall 适配器、Linux 4.19 fsnotify、4.19 SELinux policydb 克隆/交换、Manager
跨 exec fd bootstrap 和退出期调用排空。最终模块有 141 个运行时导入，目标设备的
runtime kallsyms 缺失为 0。

KPM 在这个 4.19 目标上明确关闭；vendor kernel 也没有集成 SUSFS，因此这两个功能不在
本制品承诺内。Manager 会正确显示 KPM 未启用。legacy 路径不接受运行时用户 sepolicy
替换和 init pgrp 修改，但标准 `ksu`/`ksu_file` 域及 Root 授权已验证。

## 安全规则

一个 boot 只加载一次。不要执行 `rmmod kernelsu`、`ksud unload` 或同 boot 热替换。
需要升级或模块状态异常时，普通重启后再加载新制品。操作仅限本人拥有或明确获授权的
设备，并保留恢复路径和数据备份。

SukiSU Ultra 及本派生移植继续使用 GPL-2.0；详见 `LICENSE` 和上游历史。
