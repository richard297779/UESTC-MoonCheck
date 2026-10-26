## 免编译刷写（不用装任何工具）

**先看芯片丝印再下载，两个包不能互换：**

- 芯片丝印含 **ESP32-S3**（16 MB Flash）-> `wanan-0.4.0-s3-n16r8-merged.bin`
- 芯片丝印是 **ESP32**（WROOM-32 / 4 MB Flash）-> `wanan-0.4.0-wroom32-4mb-merged.bin`

### 刷入步骤

1. 用 **Chrome / Edge**（必须支持 Web Serial）打开 <https://espressif.github.io/esptool-js/>
2. Connect 选中串口，Baudrate 先用 `921600`（卡住或报错就退回 `115200`）
3. Flash Address 填 `0x0`，文件选上面那个 `-merged.bin`
4. Program，等到日志出现 `Hash of data verified` 就已经写好了
5. **如果停在** `Hard resetting via RTS pin...`：点 **Disconnect**（或直接关掉这个网页），再按板上 RST / 拔插 USB。
   这是网页工具的收尾问题，不是刷写失败；USB 桥占着串口的 RTS 会把 EN 一直拉低，此时按 RST 是没反应的。
6. 第一次开机是**未配置**状态：合并包会擦掉 NVS。蓝牙会广播 `WANAN-XXXX`（XXXX = 蓝牙 MAC 后两字节），LED 慢闪。
   用手机 nRF Connect 或本仓库 `tools/ble_console.py` 连上，按 docs/quickstart.md 配齐（刷写细节见 docs/flashing.md）。

### 本版本资产

| 文件 | 大小 | 适用 | 用途 |
| --- | --- | --- | --- |
| wanan-0.4.0-s3-n16r8-merged.bin | 1,352,832 B | ESP32-S3 N16R8（16 MB Flash） | 整片出厂镜像，刷在 `0x0` |
| wanan-0.4.0-s3-n16r8-full.zip | 839,589 B | ESP32-S3 N16R8（16 MB Flash） | 四文件+FLASH.txt，升级只刷 `0x10000` |
| wanan-0.4.0-wroom32-4mb-merged.bin | 1,469,392 B | 经典 ESP32 WROOM-32（4 MB Flash） | 整片出厂镜像，刷在 `0x0` |
| wanan-0.4.0-wroom32-4mb-full.zip | 908,623 B | 经典 ESP32 WROOM-32（4 MB Flash） | 四文件+FLASH.txt，升级只刷 `0x10000` |
| MoonCheck-1.0.0-windows-x64.exe | 12,652,319 B | Windows 10/11 x64 | 双击 = 图形界面；命令行子命令 = CLI |
| MoonCheck-1.0.0-linux-x86_64 | 13,776,992 B | x86-64 Linux | 下载后 `chmod +x` 再运行 |

### 只想升级固件？

下载 `-full.zip`，**只**把里面的 `firmware.bin` 填到地址 `0x10000` 刷入 —— WiFi、账号、坐标、日程都会保留。
刷 `0x0` 的合并包等于整片重置，配置会全部丢失。

### 桌面版（不需要开发板，电脑上直接跑）

- `MoonCheck-1.0.0-windows-x64.exe` —— Windows 10/11 64-bit：双击 = 图形界面；带命令行参数 = CLI。首次运行在 exe 同目录生成 `config.json`
- `MoonCheck-1.0.0-linux-x86_64` —— x86-64 Linux：下载后先 `chmod +x MoonCheck-1.0.0-linux-x86_64`，用法与 Windows 版一致
- 多账号、打卡时间窗、每人独立推送与固件同语义；使用说明见仓库 `desktop/README.md`

### SHA256

```
2d489ba2f456ccd4de153323a3de538713968f12a6308016c2a36b38e51ec432  MoonCheck-1.0.0-linux-x86_64
0710d527609ef2fa3772cc293c673d62e6c854a73b9405400f670d53c6733309  MoonCheck-1.0.0-windows-x64.exe
56749f140a5a0645e4e411b5a65091d91ccc5214df078e92ce718886409cdb02  wanan-0.4.0-s3-n16r8-full.zip
8b077879121296b51a2a77d872c3b7b0aac5d6f3da989304a4c4e08c26fd55d6  wanan-0.4.0-s3-n16r8-merged.bin
78a79ecb817e983d615a84bf14f4f84595d5adc0c6af46d365b4e316445003c8  wanan-0.4.0-wroom32-4mb-full.zip
0be32161b3925e00b84888c87f01b74f86146e920c47c4dc38ddf3951445e69f  wanan-0.4.0-wroom32-4mb-merged.bin
```

### 本次构建信息

- 固件版本：`0.4.0`（取自 `firmware/include/app_version.h` 的 `FW_VERSION`，由打包脚本自动读取）
- 打包方式：`make_release.ps1`（主工程 `tools/`，公开副本 `dev/tools/`；**只消费已有 build 产物，不重新编译**）
- 桌面版：v1.0.0（PyInstaller 单文件，Windows/Linux 各自平台构建后随包发布）
- 已知工具链：platform espressif32@7.1.3 / PlatformIO Core 6.2.0 / esptool 4.11.0 / NimBLE-Arduino 2.5.1 / ArduinoJson 7.4.3
- 详细图文说明：docs/flashing.md（网页刷写）与 docs/quickstart.md（蓝牙配置）

> 打卡时间窗 21:00-23:30，登录只在窗口内发生；配置完成后 `auto on` 才会每天自动执行。
