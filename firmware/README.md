# 自行编译固件

> Releases 中现有的 v0.4.0 预编译固件不支持平台新增的微信操作票据要求。要让设备按当前平台要求打卡，请从当前源码编译后再[刷入固件](../docs/flashing.md)；待发布包含此能力的新固件后，可直接下载对应板型的发布包。

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

每个使用中的账号都需要本人授权的微信凭据（`openid`、`access`、`refresh`、`expiresAt`，Unix 秒）。仓库目前不包含获取初始凭据的工具；请勿使用他人凭据。通过 `ble_console.py` 的 `raw` 命令导入并测试：

```text
raw {"op":"wx.import","slot":0,"openid":"<本人openid>","access":"<access token>","refresh":"<refresh token>","expiresAt":1792890000}
raw {"op":"wx.status"}
raw {"op":"wx.test","slot":0}
```

账号表为空时设备主账号使用槽位 0；账号表非空时，微信凭据槽位按账号表顺序对应（从 0 开始）。凭据保存在 NVS，刷新后会自动保存；刷新被拒绝时需由账号本人重新获取并导入。`chain` 只验证 CAS / 微应用链路，不能代替 `raw {"op":"wx.test","slot":0}`。

> Releases 中的 v0.4.0 预编译固件早于此改动，不支持 `wx.*` 指令。当前源码的固件版本字符串仍为 `0.4.0`，请从当前源码编译，不要仅凭版本号判断功能。

## 4. 首次上电的日志

```text
I main: === WANAN checkin device booting ===
I main: firmware 0.4.0, built ...
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
