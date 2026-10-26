# 自行编译固件

> 发行版本 v0.7.0，修正版已获用户实测可行确认，具体测试板型未说明，新增四型号未经实机验证；详见[板型支持矩阵](../docs/board-support.md)。当前 Latest 为 v0.7.0。可直接下载[发行包](https://github.com/richard297779/UESTC-MoonCheck/releases/tag/v0.7.0)，或按本文自行构建。发行包同时附带 Android BLE 控制端测试 APK。

---

## 需要什么

| 项目 | 说明 |
| --- | --- |
| 电脑 | Windows / macOS / Linux |
| 软件 | [VSCode](https://code.visualstudio.com/) + [PlatformIO 插件](https://platformio.org/install/ide?install=vscode) |
| 网络 | 首次构建要下载平台与工具链，约 1–2 GB |
| 硬件 | 板子 + 数据线 |

经典 ESP32（WROOM-32）还需要另一套工具链，先装好再打开工程，否则会一直卡在加载：

```powershell
pio pkg install -g -p "platformio/toolchain-xtensa-esp32"
```

> 源码注释里引用的接口契约、抓包记录等设计文档见 [开发者文档](../dev/README.md)，不影响编译。

---

## 1. 打开工程

用 VSCode 打开 **`firmware/` 这个文件夹**（不是仓库根目录），PlatformIO 会自动识别。

左侧 PlatformIO 面板 → **Project Tasks**，先选环境：

| 环境名 | 适用板型 | Flash | 状态灯 |
| --- | --- | --- | --- |
| `esp32-s3-devkitc-1` | ESP32-S3-N16R8 | 16 MB | WS2812 @ GPIO48 |
| `esp32-wroom-32` | ESP32-WROOM-32 | 4 MB | 普通 LED @ GPIO2 |
| `esp32-wrover-4mb` | WROVER，8 MB QSPI PSRAM | 4 MB | 默认关闭，实验目标 |
| `esp32-s3-n8` | S3，无 PSRAM | 8 MB | GPIO48 RGB，实验目标 |
| `esp32-s3-n8r8` | S3，8 MB OPI PSRAM | 8 MB | GPIO48 RGB，实验目标 |
| `esp32-c3-devkitm-1` | 官方 C3-DevKitM-1 | 4 MB | GPIO8 RGB，实验目标 |

> 多块 WROOM 板可共用此环境；上传时用 `--upload-port COMx` 指定实际串口。`platformio.debug.ini` 另提供绑定 COM6/COM7 的调试环境。

## 2. 编译并烧录

图形界面：选好 env → **Build** → **Upload** → **Monitor**。

命令行等价：

```powershell
cd firmware
pio run -e esp32-s3-devkitc-1 -t upload      # ESP32-S3
pio run -e esp32-wroom-32 -t upload --upload-port COM6  # 经典 ESP32
pio device monitor --port COM5               # 串口日志，115200
```

只有一种固件：**打卡几个人由账号表决定**，不需要为了多人重新编译。

## 3. 微信操作票据与凭据配置

平台已要求打卡请求携带 `X-Checkin-Ticket`。当前源码会在打卡前自动通过微信小程序授权链获取短效票据，不需要手工获取或填写票据本身。账号的 CAS 登录配置（学号、密码、可选浏览器指纹）仍然需要保留。

每个使用中的账号都需要本人通过 PC 端 QR Connect 扫码授权取得微信凭据。授权 URL、Cookie 获取位置及字段映射见[快速开始](../docs/quickstart.md#22-准备微信授权凭据)。Cookie 中 `openid`、`accesstoken`、`refreshtoken` 分别填入 `openid`、`access`、`refresh`；`expires_in` 是剩余秒数，需加当前 Unix 时间换算为绝对 Unix 到期时间 `expiresAt`，也可设为 `0` 令固件首次使用前刷新并保存到期时间。按账号表顺序为每位账号分别授权，切勿共用他人凭据。在 `ble_console.py` 中使用快捷命令导入并测试：

```text
wx import 0 <openid> <accesstoken> <refreshtoken> 0
wx status
wx test 0
```

账号表为空时设备主账号使用槽位 0；账号表非空时，微信凭据槽位按账号表顺序对应（从 0 开始）。凭据保存在 NVS，刷新后会自动保存；刷新被拒绝时需由账号本人重新扫码并导入。`chain` 只验证 CAS / 微应用链路，不能代替 `wx test 0`。

> v0.6.0 修复 BLE 命令在任务队列中被截到 511 字节的问题，并在 `hello` 中声明 `maxCommandBytes:4095`。旧版本缺少该能力字段时，Android 会在发送前阻止过长配置。协议版本仍为 1。

## 4. 首次上电的日志

```text
I main: === WANAN checkin device booting ===
I main: firmware 0.7.0, built ...
I logger: logger ready (ring=ok)
I app_config: config loaded: ready=0 user=unset bfp=0
I status_led: led ready: pin=48 rgb=1
I ble_service: ble ready: name=WANAN-XXXX (service 6E400001-...)
I main: wifi not configured; please provision over BLE
I main: setup done, free heap=... B, psram=...
```

未配置时 LED 蓝色慢闪。看到 `ble ready` 就可以用蓝牙配置了 → [快速开始](../docs/quickstart.md)。

## 5. 可调项

| 想改什么 | 改哪里 |
| --- | --- |
| 状态灯引脚 / 是否 RGB | `include/app_types.h` 的 `kLedPin`、`kLedIsRgb`；或编译宏 `-DWANAN_LED_PIN=2 -DWANAN_LED_IS_RGB=0`，`-DWANAN_LED_PIN=-1` 关闭 |
| 默认打卡时间 | 蓝牙 `sched` 命令即可，不必改代码 |
| 串口走外置 USB-UART（CH340/CP210x）却看不到日志 | `platformio.ini` 里把 `-DARDUINO_USB_CDC_ON_BOOT=1` 改为 `0` |
| 分区表 | `partitions/partitions_16mb.csv`（S3）、`partitions_4mb.csv`（4 MB，双槽）；**必须保持纯 ASCII**，中文注释会让构建失败 |

> 改了分区表就必须整片重刷（含 `partitions.bin`），只刷 app 会错位，配置也会丢。

## 6. 编译期常见报错

| 现象 | 处理 |
| --- | --- |
| 切换 WROOM env 后一直 loading | 工具链没装，见上面的 `pio pkg install` |
| `ble_service.cpp` 回调签名报错 | NimBLE-Arduino 版本差异：`lib_deps` 改为 `h2zero/NimBLE-Arduino @ ^1.4.3`，并去掉回调里的 `NimBLEConnInfo&` 参数 |
| `rgbLedWrite` 未定义 | Arduino-ESP32 < 3.x：把 platform 换成 `espressif32@^6.5.0`，或改用 `neopixelWrite` |
| PSRAM 报错 / 容量不对 | 先 `esptool.py flash_id` 核实；QSPI PSRAM 把 `board_build.arduino.memory_type` 改成 `qio_qspi` |
| 串口完全没输出 | 换 USB 口 / 数据线；确认 `upload_port` 正确；见下一节 |

## 7. 烧录失败

**最常见的原因是没进入下载模式。** 按顺序试：

1. **手动进下载模式**：按住 **BOOT(IO0)** → 点一下 **RST(EN)** → 松开 BOOT → 立刻 Upload。
2. **释放串口占用**：关掉 PlatformIO Monitor、其他串口助手、浏览器里的 Web Serial 页面，
   结束残留的 `python.exe` / `esptool` 进程，然后拔插 USB 再试。
3. **物理层**：直插主板 USB 口（不过 HUB），换能传数据的线；
   板子有两个 USB-C 口时两个都试（`UART` 口走板载 USB-UART，通常最稳，此时要把 `-DARDUINO_USB_CDC_ON_BOOT` 改成 `0`）。
4. **兜底**：手动进入下载模式后用 esptool 直接刷：

```powershell
& "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32s3 --port COM4 --baud 115200 `
  --before no_reset --after hard_reset write_flash -z `
  0x0000  .pio\build\esp32-s3-devkitc-1\bootloader.bin `
  0x8000  .pio\build\esp32-s3-devkitc-1\partitions.bin `
  0x10000 .pio\build\esp32-s3-devkitc-1\firmware.bin
```

## 8. 安全说明

- `auth.set` 只写不读；`status` 里学号打码，密码和指纹只显示「是否已设置」；`wx.status` 不回显微信令牌。
- 日志中的 Cookie / 密码 / 验证码会被自动脱敏，不会明文输出。
- 设备在 NVS 中**明文**保存学号、密码和微信授权凭据，别把板子转借他人；设备丢失后立即撤销/更新相关凭据。

## v0.7.0 连接回执修正版

2026-10-27 修复 APK 连接时“设备回执不完整”的固件缺陷：ArduinoJson 序列化清空已有 String，导致状态外层丢失和多余括号。现先单独序列化 runtime 再拼接，补充生产组包函数回归。版本为 0.7.0，修复联调已获用户确认，构建时间和 SHA-256 已更新；现有 APK 无需重装。

已有同板型、同分区固件请解压新 full.zip，只刷 firmware.bin 到 `0x10000`，保留配置。首次安装或需要清配置才刷 merged.bin 到 `0x0`。

## v0.7.0 运行状态

`status` 新增 `runtime`（内部堆当前/最低可用、分配器使用峰值、最大连续块、PSRAM、网络任务栈最低余量、开机秒数、boardId）及 `net.ssid`（实际连接网络）。统计重启后重新开始，内部堆不含 PSRAM。`logs` 限制 1–60 行，固件与 APK 均隐藏敏感日志；Android 运行状态页可查看。新环境构建结束生成 `board-build.json` 供打包交叉核对。
