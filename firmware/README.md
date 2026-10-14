# 晚安成电自动打卡终端 · 固件工程

> ESP32-S3-N16R8 / PlatformIO + Arduino-ESP32 3.x
> 模块功能与接口见 `../docs/firmware-design.md`；接口契约见 `../docs/api-contract.md`。

---

## 1. 目录结构

```
firmware/
├─ platformio.ini              # 工程配置（板级参数、依赖、串口）
├─ partitions/partitions_16mb.csv
├─ include/
│  ├─ app_types.h              # 共享类型 / 错误码 / 编译期常量（无 .cpp）
│  ├─ app_version.h            # 版本号
│  ├─ uestc_ca_pem.h           # 由 tools/gen_ca_pem.js 生成的真实 CA 链
│  ├─ app_config.h    logger.h    status_led.h   wifi_manager.h
│  ├─ time_service.h  cookie_jar.h http_client.h crypto_util.h
│  ├─ cas_auth.h      checkin_api.h checkin_service.h
│  ├─ task_scheduler.h ble_service.h command_router.h
└─ src/                        # 与上面每个 .h 一一对应的 .cpp + main.cpp
```

## 2. 编译与烧录（**同一份代码，多个构建目标**）

| 环境名（env） | 用途 | 板子 | Flash / PSRAM | 分区表 | 状态 LED |
| --- | --- | --- | --- | --- | --- |
| `esp32-s3-devkitc-1` | **单人版**（`default_envs`） | ESP32-S3-N16R8 | 16 MB / 8 MB OPI | `partitions_16mb.csv` | WS2812 @ GPIO48 |
| `esp32-s3-multiuser` | **多人顺序打卡版**（同一块 S3，只多 `-DWANAN_MULTIUSER=1`） | ESP32-S3-N16R8 | 16 MB / 8 MB OPI | `partitions_16mb.csv` | 同上 |
| `esp32-wroom-32`（示例见 `platformio.wroom32.ini.example`） | 经典 ESP32-WROOM-32 | 4 MB / 无 | `partitions_4mb.csv` | 普通 LED @ GPIO2 |

> 单人版与多人版**共享全部代码**，只多一个编译宏（影响：多人开关默认值 + 版本号后缀 `-multi`）。
> 两者都能用 `multi.set` 现场切换 ⇒ **回退到单人行为不需要重刷**。多人设计见 `docs/multi-user-design.md`。

> ⚠️ 经典 ESP32 需要另一套工具链 `toolchain-xtensa-esp32`（约 100 MB）。未安装时 VSCode 展开该
> env 的任务树会**静默下载**（表现为一直 loading task）。先手动装好即可：
> `pio pkg install -g -p "platformio/toolchain-xtensa-esp32"`；或把
> `platformio.wroom32.ini.example` 粘进 `platformio.ini` 后再 Reload。

1. VSCode 打开 **`firmware/` 这个文件夹**（不是仓库根目录），PlatformIO 会自动识别工程；
2. **首次构建会下载 PlatformIO Core + espressif32 平台 + 对应工具链**（经典 ESP32 与 S3 是两套 xtensa 工具链）；
3. **切换目标**：左侧 PlatformIO 图标 → **Environment 选择器** → 选一个 env；此后 `Build` / `Upload` / `Monitor` 只作用于它；
4. 串口默认自动探测；**多块板同时插着时务必写死端口**（对应 env 里 `upload_port = COMx`，或命令行 `--upload-port`），排障见 §9。S3 走原生 USB，经典 ESP32 走 UART0（需外接 CP2102/CH340）；
5. **LED 不同不用改代码**：加编译标志 `-DWANAN_LED_PIN=2 -DWANAN_LED_IS_RGB=0`，或 `-DWANAN_LED_PIN=-1` 彻底关掉；
6. **无 PSRAM 的板子（WROOM-32）自动降级**：日志环形缓冲退回内部堆（`logger.cpp` 已有回退），功能完全一致。

命令行等价：

```powershell
pio run -e esp32-s3-devkitc-1 -t upload    # 单人版
pio run -e esp32-s3-multiuser  -t upload   # 多人版（同一块 S3）
pio run -e esp32-wroom-32 -t upload --upload-port COM6     # 指定端口，避免刷错板
pio device monitor --port COM5             # 串口 115200
```

## 3. 上电后的预期输出

```
[.. ] I main: === WANAN checkin device booting ===
[.. ] I main: firmware 0.1.0, built ...
[.. ] I logger: logger ready (ring=ok)
[.. ] I app_config: config loaded: ready=0 user=unset bfp=0
[.. ] I status_led: led ready: pin=48 rgb=1
[.. ] I ble_service: ble ready: name=WANAN-XXXX (service 6E400001-...)
[.. ] I main: wifi not configured; please provision over BLE
[.. ] I main: setup done, free heap=... B, psram=...
```

未配网时 LED 蓝色慢闪。

## 4. 蓝牙配网（手机 nRF Connect）

1. 连接广播名 `WANAN-XXXX`；
2. 展开 **Nordic UART Service**（`6E400001-…`），订阅 **TX**（`6E400003-…`，Notify）；
3. 向 **RX**（`6E400002-…`）写入下面命令（**每条以换行结尾**，UTF-8）：

```jsonc
{"op":"hello","id":1}
{"op":"wifi.set","id":2,"ssid":"宿舍WiFi名","pass":"密码"}
{"op":"auth.set","id":3,"user":"2023XXXXXXXXX","pass":"统一身份认证密码","bfp":"抓包里的32位指纹"}
{"op":"loc.set","id":4,"lat":30.75498,"lng":103.933883,"addr":"四川省成都市郫都区"}   // 换成你自己宿舍的实测坐标
{"op":"sched.set","id":5,"at":"22:00","window_end":"23:30","jitter_s":30}
```

> `bfp` 是免二次验证的关键（契约 §8.3）。留空也能登录，但服务端会要求验证码。

## 5. 自测指令（**非打卡时间也能验证前置功能**）

| 指令 | 作用 | 何时可用 |
| --- | --- | --- |
| `{"op":"test.http","id":10,"url":"https://idas.uestc.edu.cn/authserver/login?service=https%3A%2F%2Fjzsz.uestc.edu.cn%2F"}` | DNS+TLS+HTTP 链路，返回 `status/bytes/ms/hasSalt` | 随时 |
| `{"op":"test.time","id":11}` | 设备时间、是否在窗口内、下次计划时间 | 随时 |
| `{"op":"test.login","id":12}` | 取登录页→AES 加密→提交→换 SESSION，返回 `mfa/session` | 随时 |
| `{"op":"test.probe","id":13}` | **前置体检**：会话/今日状态/平台时段/打卡点，只读 | 随时 |
| `{"op":"test.checkin","id":14}` | 打卡（先查状态，幂等） | **21:00–23:30** |
| `{"op":"test.checkin","id":15,"force":true}` | 跳过"今日已打卡"检查（仍需在时段内） | 21:00–23:30 |
| `{"op":"logs","id":16,"n":50}` | 拉取最近日志（已脱敏） | 随时 |
| `{"op":"status","id":17}` | 配置摘要 + 网络 + Cookie + 下次计划 | 随时 |
| `{"op":"session.clear","id":18}` | 清空 Cookie（模拟会话失效） | 随时 |
| `{"op":"factory.reset","id":19,"confirm":"YES"}` | 擦除配置并重启 | 随时 |

期望回执示例：

```json
{"id":13,"evt":"ok","msg":"probe done","data":{"sessionValid":true,
 "deviceTime":"2026-10-11 19:32:07","window":"21:00 - 23:30","inWindow":false,
 "windowFromPlatform":true,"recordStatusCode":"PENDING","checkinTime":"",
 "distanceMeter":-1.0,"abnormalFlag":false,"location":"本科3组团","radiusMeter":130,
 "elapsedMs":842}}
```

## 6. PC 端蓝牙调试台（推荐，免得在手机上敲 JSON）

```powershell
python -m pip install bleak
python ..\tools\ble_console.py            # 自动扫描 WANAN-* 并连接
python ..\tools\ble_console.py --addr XX:XX:XX:XX:XX:XX
```

交互命令见 `tools/ble_console.py --help`（内置 `probe`、`login`、`checkin`、`time`、`http <url>` 等快捷方式）。

## 7. 首次构建可能需要的微调

| 现象 | 处理 |
| --- | --- |
| 串口无任何输出 | 若板子用的是外置 USB-UART 芯片（CH340/CP210x）而非原生 USB，把 `platformio.ini` 中 `-DARDUINO_USB_CDC_ON_BOOT=1` 改为 `0` |
| 灯不亮或颜色不对 | 改 `include/app_types.h` 里 `kLedPin`（默认 48）与 `kLedIsRgb`（默认 true） |
| `ble_service.cpp` 报回调签名错误 | NimBLE-Arduino 版本差异：把 `lib_deps` 改为 `h2zero/NimBLE-Arduino @ ^1.4.3`，并把两个回调签名里的 `NimBLEConnInfo&` 参数去掉 |
| PSRAM 相关报错 / 容量不对 | 用 `pio run -t upload -t monitor` 前先 `esptool.py flash_id` 核实；QSPI PSRAM 把 `board_build.arduino.memory_type` 改成 `qio_qspi` |
| 编译报 `rgbLedWrite` 未定义 | 说明 Arduino-ESP32 版本 < 3.x：`platform = espressif32@^6.5.0` 或把 `status_led.cpp` 中的分支改成 `neopixelWrite` |

## 8. 安全提示

- `auth.set` 只写不读，`status` 里学号打码、密码只报"是否存在"；
- 日志经过 `Logger::redact()`，Cookie/密码/验证码不会明文落盘；
- 设备内 NVS 明文保存凭据（见设计文档 §8），后续可开 Flash 加密。

## 9. 烧录失败排障（按顺序试）

| 报错 | 含义 | 处理 |
| --- | --- | --- |
| `Failed to connect to ESP32-S3: Invalid head of packet (0x08)` | 端口能开、有数据，但对方不是 ROM 下载模式（板内已有固件在打日志，或端口被别的程序占用） | 见下面 ①②③ |
| `Failed to connect ... No serial data received` | 完全没回应 | 换线/换口/确认端口号正确 |
| `Serial port COMx` 后长时间无输出 | 端口被占用 | 关闭所有串口监视器/其它 VS Code 窗口，拔插 USB |

**① 手动进入下载模式（最有效）**

按住板上 **BOOT(IO0)** 键 → 点按一下 **RST(EN)** 键 → 松开 BOOT → 立刻点 Upload。
此时芯片停在 ROM 下载模式，不会再被旧固件的串口输出干扰。

**② 释放端口占用**

- 关掉 PlatformIO 的 **Monitor** 窗口、其它串口助手、Arduino IDE、浏览器 WebSerial 页面；
- 只保留一个 VS Code 窗口；
- 任务管理器里若残留 `python.exe` / `esptool` 进程，结束它；
- 然后拔掉 USB 重新插入，等 2 秒再 Upload。

**③ 物理层**

- 直接插**主板**的 USB 口，不要经过 HUB；
- 换一根能传数据的 USB-C 线（充电线不行）；
- 若板子有两个 USB-C 口（`USB` 与 `UART`），两个都试一下：`UART` 口走板载 USB-UART 芯片，通常最稳（此时要把 `platformio.ini` 的 `-DARDUINO_USB_CDC_ON_BOOT` 改为 `0`，否则串口日志会走原生 USB 而看不到）；
- 端口号变了就同步修改 `platformio.ini` 的 `upload_port` / `monitor_port`，或注释掉让 PlatformIO 自动探测。

**④ 兜底（手动进下载模式 + 免复位烧录）**

```powershell
# 先按 ① 进入下载模式，然后：
& "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32s3 --port COM4 --baud 115200 ^
  --before no_reset --after hard_reset write_flash -z ^
  0x0000  .pio\build\esp32-s3-devkitc-1\bootloader.bin ^
  0x8000  .pio\build\esp32-s3-devkitc-1\partitions.bin ^
  0x10000 .pio\build\esp32-s3-devkitc-1\firmware.bin
```

（`0x8000` 是分区表偏移；本工程分区表就放在该位置。若报分区表偏移不符，以 `partitions_16mb.csv` 为准。）
