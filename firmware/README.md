# 自行编译固件

> 如果你只是想把设备跑起来，**不需要看这一页**——直接下载现成固件刷入即可：
> 见 [刷固件](../docs/flashing.md)。只有想改代码、改参数，或没有对应板型的现成固件时，才需要自行编译。

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
| `esp32-wroom-32-COM6` / `-COM7` | ESP32-WROOM-32 | 4 MB | 普通 LED @ GPIO2 |

> 换了端口就改 `platformio.ini` 里对应 env 的 `upload_port`，或者用下面的命令行加 `--upload-port COMx`。

## 2. 编译并烧录

图形界面：选好 env → **Build** → **Upload** → **Monitor**。

命令行等价：

```powershell
cd firmware
pio run -e esp32-s3-devkitc-1 -t upload      # ESP32-S3
pio run -e esp32-wroom-32-COM6 -t upload     # 经典 ESP32（端口写在 env 里）
pio device monitor --port COM5               # 串口日志，115200
```

只有一种固件：**打卡几个人由账号表决定**，不需要为了多人重新编译。

## 3. 首次上电的日志

```text
I main: === WANAN checkin device booting ===
I main: firmware 0.3.1, built ...
I logger: logger ready (ring=ok)
I app_config: config loaded: ready=0 user=unset bfp=0
I status_led: led ready: pin=48 rgb=1
I ble_service: ble ready: name=WANAN-XXXX (service 6E400001-...)
I main: wifi not configured; please provision over BLE
I main: setup done, free heap=... B, psram=...
```

未配置时 LED 蓝色慢闪。看到 `ble ready` 就可以用蓝牙配置了 → [快速开始](../docs/quickstart.md)。

## 4. 可调项

| 想改什么 | 改哪里 |
| --- | --- |
| 状态灯引脚 / 是否 RGB | `include/app_types.h` 的 `kLedPin`、`kLedIsRgb`；或编译宏 `-DWANAN_LED_PIN=2 -DWANAN_LED_IS_RGB=0`，`-DWANAN_LED_PIN=-1` 关闭 |
| 默认打卡时间 | 蓝牙 `sched` 命令即可，不必改代码 |
| 串口走外置 USB-UART（CH340/CP210x）却看不到日志 | `platformio.ini` 里把 `-DARDUINO_USB_CDC_ON_BOOT=1` 改为 `0` |
| 分区表 | `partitions/partitions_16mb.csv`（S3）、`partitions_4mb.csv`（4 MB，双槽）；**必须保持纯 ASCII**，中文注释会让构建失败 |

> 改了分区表就必须整片重刷（含 `partitions.bin`），只刷 app 会错位，配置也会丢。

## 5. 编译期常见报错

| 现象 | 处理 |
| --- | --- |
| 切换 WROOM env 后一直 loading | 工具链没装，见上面的 `pio pkg install` |
| `ble_service.cpp` 回调签名报错 | NimBLE-Arduino 版本差异：`lib_deps` 改为 `h2zero/NimBLE-Arduino @ ^1.4.3`，并去掉回调里的 `NimBLEConnInfo&` 参数 |
| `rgbLedWrite` 未定义 | Arduino-ESP32 < 3.x：把 platform 换成 `espressif32@^6.5.0`，或改用 `neopixelWrite` |
| PSRAM 报错 / 容量不对 | 先 `esptool.py flash_id` 核实；QSPI PSRAM 把 `board_build.arduino.memory_type` 改成 `qio_qspi` |
| 串口完全没输出 | 换 USB 口 / 数据线；确认 `upload_port` 正确；见下一节 |

## 6. 烧录失败

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

## 7. 安全说明

- `auth.set` 只写不读；`status` 里学号打码，密码和指纹只显示「是否已设置」。
- 日志中的 Cookie / 密码 / 验证码会被自动脱敏，不会明文输出。
- 设备在 NVS 中**明文**保存凭据，别把板子转借他人；丢失后立即改密码。
