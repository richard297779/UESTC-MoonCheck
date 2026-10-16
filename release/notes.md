## 免编译刷写（不需要装任何东西）

**先确认你的板子型号**（看芯片丝印，或看开发板标签）：
- 芯片上写 **ESP32-S3** → 下载 `wanan-0.3.1-s3-n16r8-merged.bin`
- 芯片上写 **ESP32**（WROOM-32） → 下载 `wanan-0.3.1-wroom32-4mb-merged.bin`

### 刷入步骤
1. 用 **Chrome 或 Edge**（必须支持 Web Serial）打开官方网页烧录工具：
   <https://espressif.github.io/esptool-js/>
2. 板子用 **USB 数据线**（不是只能充电的线）插电脑。
3. 网页里波特率选 `921600`（连不上就改 `115200`）→ **Connect** → 选串口。
   如果连不上：按住板上的 **BOOT** 不放，点一下 **RESET/EN**，松开 BOOT，再点 Connect。
4. 文件表里**只填一行**：地址 `0x0`，文件选刚下载的 `.bin`。
   Flash Mode / Frequency / Size 选 `keep` / `detect`（固件头里已经带好正确参数）。
5. **Program** → 等进度条走完 → 板子自动重启。

### ⚠️ 合并包会清空已配置的数据
`-merged.bin` 是**出厂镜像**，段间空洞会被填成 `0xFF`，其中包含 NVS（`0x9000`）：
WiFi、账号表、坐标、通知地址都会回到默认值。
**只是想升级固件？** 请下载 `-full.zip`，只把里面的 `firmware.bin` 填到地址 `0x10000` 刷入，
其余三个文件不要动 —— 配置全部保留。

### 刷完之后要做的事（BLE 配置，只影响首次使用）
网页烧录**不负责配置**。还需要一台电脑（或安卓手机 + nRF Connect）通过蓝牙把
校园网、账号、坐标、打卡时间写进设备，见仓库 `docs/release-guide.md` 第 7 节 / `docs/quickstart.md`。

### 本次版本
固件 `0.3.1`（单一固件：打卡几个人由账号表决定，`account add` / `account on|off`，表为空则用设备自身账号）
构建环境：PlatformIO Core 6.2.0 / platform espressif32 **7.1.3** / Arduino-ESP32 4.2.17
/ esptool 4.11.0 / NimBLE-Arduino 2.5.1 / ArduinoJson 7.4.3（源码用同样版本可复现，校验见 `sha256.txt`）

### 免责
本固件仅供你在**自己或已知情同意**的同学账号上使用，用于校园服务平台的定位签到。
请自行确认符合学校相关规定；因使用产生的账号风险由使用者自担。