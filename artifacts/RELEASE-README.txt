XPad2 SukiSU Ultra late-load v0.1.0

Exact target only:
  device: TALIH_PD2 / ls12_mt8797_wifi_64
  firmware: Android 13 /260
  kernel: 4.19.191+
  KMI: xpad2-sukisu-4.19.191
  SukiSU/Manager version code: 40796

Files:
  sukisu-xpad2-4.19.191.ko        standalone XPad2 kernel module
  ksud-sukisu-xpad2              loader with the exact module embedded
  SHA256SUMS                      integrity manifest

Manager:
  Download the official production-signed v4.1.3 (40796) APK from:
  https://github.com/SukiSU-Ultra/SukiSU-Ultra/releases/tag/v4.1.3
  SHA-256: 1b1e837c0a5b6aa34554882fad67cef6db6ca1a84d43e07dd904cf54f8d261ae

The loader verifies the official com.sukisu.ultra signing certificate before
pinning its appId. A missing or untrusted Manager is non-blocking for module
load but will not receive the early identity pin.

After authorized temporary root:
  ksud-sukisu-xpad2 late-load --kmi xpad2-sukisu-4.19.191

Do not pass module-spoofing or allow-shell flags. Do not unload or replace the
module in a live boot. Ordinary reboot before every upgrade or retry.

KPM and SUSFS are intentionally unavailable on this Linux 4.19 target.
See the repository README.md and PROGRESS.md for build and validation details.
