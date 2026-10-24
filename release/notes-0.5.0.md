## v0.5.0 固件更新

- 微信授权链现在会在设备端自动换取并附加平台要求的短效 `X-Checkin-Ticket`。
- 新增 `wx.import`、`wx.status`、`wx.test` BLE 指令；授权凭据按账号槽位持久化。
- 本次发行仅包含 ESP32 固件，不包含桌面版可执行文件。

## 免编译刷写（不用装任何工具）

**先看芯片丝印再下载，两个包不能互换：**

- 芯片丝印含 **ESP32-S3**（16 MB Flash）-> `wanan-0.5.0-s3-n16r8-merged.bin`
- 芯片丝印是 **ESP32**（WROOM-32 / 4 MB Flash）-> `wanan-0.5.0-wroom32-4mb-merged.bin`

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
| wanan-0.5.0-s3-n16r8-merged.bin | 1,450,704 B | ESP32-S3 N16R8（16 MB Flash） | 整片出厂镜像，刷在 `0x0` |
| wanan-0.5.0-s3-n16r8-full.zip | 900,966 B | ESP32-S3 N16R8（16 MB Flash） | 四文件+FLASH.txt，升级只刷 `0x10000` |
| wanan-0.5.0-wroom32-4mb-merged.bin | 1,567,632 B | 经典 ESP32 WROOM-32（4 MB Flash） | 整片出厂镜像，刷在 `0x0` |
| wanan-0.5.0-wroom32-4mb-full.zip | 969,723 B | 经典 ESP32 WROOM-32（4 MB Flash） | 四文件+FLASH.txt，升级只刷 `0x10000` |

### 只想升级固件？

下载 `-full.zip`，**只**把里面的 `firmware.bin` 填到地址 `0x10000` 刷入 —— WiFi、账号、坐标、日程都会保留。
刷 `0x0` 的合并包等于整片重置，配置会全部丢失。


### SHA256

```
1ea108ab7d3ea2a4bed56daa626c2a319973ff9c7ef28f99d25f39e0af382dfd  wanan-0.5.0-s3-n16r8-full.zip
0d7e190e1656f1fa97202df8826e2a025fa36f4d33dcc7504c3bc1d92da64880  wanan-0.5.0-s3-n16r8-merged.bin
5177e709762a1b3f240ac1f3c91f91c433b020400e91ab35e0daaaaa56418597  wanan-0.5.0-wroom32-4mb-full.zip
89a0c6dccf5c0898cd5ea360fb71b4216ba99dc862cd55557710dc71bf43a4b7  wanan-0.5.0-wroom32-4mb-merged.bin
```

### 本次构建信息

- 固件版本：`0.5.0`（取自 `firmware/include/app_version.h` 的 `FW_VERSION`，由打包脚本自动读取）
- 打包方式：`make_release.ps1`（主工程 `tools/`，公开副本 `dev/tools/`；**只消费已有 build 产物，不重新编译**）

- 已知工具链：platform espressif32@7.1.3 / PlatformIO Core 6.2.0 / esptool 4.11.0 / NimBLE-Arduino 2.5.1 / ArduinoJson 7.4.3
- 详细图文说明：docs/flashing.md（网页刷写）与 docs/quickstart.md（蓝牙配置）

> 打卡时间窗 21:00-23:30，登录只在窗口内发生；配置完成后 `auto on` 才会每天自动执行。
