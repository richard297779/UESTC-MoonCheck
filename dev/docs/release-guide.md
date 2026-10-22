# 发行与免编译刷写指南

> 目标：你在本地构建一次，把产物发到 GitHub Release；**其他人不用装 PlatformIO、不用 build，
> 打开乐鑫官方网页烧录工具就能把固件刷进板子**。
>
> 全流程只有两条链路：
>
> ```
> 【发布者】锁版本 → 本地构建 → 合并/打包 → 发 Release（一次）
> 【使用者】确认板子型号 → 下载对应资产 → 网页刷入 → 蓝牙配置 8 项 → 当晚自动打卡（每次）
> ```
>
> 本文所有偏移、大小、命令都是在本仓库当前代码上**实测**得到的（esptool v4.11.0 / PlatformIO Core 6.2.0），
> 可以直接复制执行。

---

## 1. 先看懂三张表

### 1.1 两个型号的烧录参数（**不可互换**）

| 项目 | ESP32-S3-N16R8 | ESP32-WROOM-32（经典 ESP32） |
| --- | --- | --- |
| PlatformIO 环境 | `esp32-s3-devkitc-1` | `esp32-wroom-32-COM6` / `-COM7` |
| esptool `--chip` | `esp32s3` | `esp32` |
| **bootloader 偏移** | **`0x0`** | **`0x1000`** ← 最容易搞错的一处 |
| partitions.bin | `0x8000` | `0x8000` |
| boot_app0.bin | `0xe000` | `0xe000` |
| firmware.bin（app） | `0x10000` | `0x10000` |
| Flash Mode / Freq | `dio` / `80m` | `dio` / `40m` |
| Flash Size | `16MB` | `4MB` |
| 分区表 | `firmware/partitions/partitions_16mb.csv` | `partitions_4mb.csv`（双槽 `0x1F0000`） |

> ⚠️ 两个型号的 `bootloader.bin` 内容不同（15,104 B vs 17,536 B，编码了各自的 freq/size），
> **刷错型号的 bootloader 会直接起不来**。所以 Release 必须按板子出两套资产。
>
> ⚠️ `boot_app0.bin` 不在构建目录里，它来自框架包：
> `~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin`
> （Windows：`C:\Users\<你>\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin`）

### 1.2 使用者要下载什么

| 场景 | 下载 | 刷哪里 | 会不会丢已配好的 WiFi/账号 |
| --- | --- | --- | --- |
| **第一次拿到板子**（出厂镜像） | `wanan-<ver>-<板型>-merged.bin` | 一个文件，地址 `0x0` | **一定会清空**（见 1.3） |
| **升级固件**（已经配好了） | `wanan-<ver>-<板型>-app.zip` 里的 `firmware.bin` | 地址 `0x10000` | 不会（nvs 在 `0x9000`，不碰） |
| 换了分区表（例如 4 MB 从旧表改成双槽 `0x1F0000`） | `-full.zip` 四个文件 | 按 1.1 的偏移表 | 不会（nvs 偏移没变） |

### 1.3 为什么合并包一定会清配置（实测）

`merge_bin` 把四段拼成一个从 `0x0` 开始的镜像，**段与段之间的空洞填 `0xFF`**。
逐字节检查合并包：

```
0x8000  aa50 0102 ...          ← 分区表（正常）
0x9000  ffff ffff ffff ...     ← NVS 起点：全 FF
0xB000  ffff ffff ffff ...     ← NVS 中段：全 FF
```

全 `0xFF` = 空 flash = **NVS 命名空间不存在** → WiFi、账号表、坐标、通知地址全部回默认值。
这不是 bug，是合并包的固有行为，所以**必须在 Release 说明里写清楚**：合并包 = 出厂镜像，升级请用 app 单文件。

---

## 2. 【发布者】第 0 步：先锁版本（发预编译产物前必做）

预编译产物的可信度 = 别人能不能复现。当前 `firmware/platformio.ini` 有三处「会随时间漂移」的写法，
发布前建议改成（改完重新构建一次，产物必须字节级或至少功能一致）：

```ini
; ---- 改前 ----
platform = espressif32
lib_deps =
  h2zero/NimBLE-Arduino @ ^2.1.0
  bblanchon/ArduinoJson @ ^7.2.0

; ---- 改后（本机实测版本，2026-10 验证通过）----
platform = espressif32@7.1.3
framework-arduinoespressif32 由该 platform 决定 = 4.20017.260907+sha.dcc1105b（即 Arduino-ESP32 4.2.17 线）
lib_deps =
  h2zero/NimBLE-Arduino @ 2.5.1
  bblanchon/ArduinoJson @ 7.4.3
```

本机实测的工具链版本（写进 Release 说明，方便别人核对）：

| 组件 | 版本 |
| --- | --- |
| PlatformIO Core | 6.2.0 |
| platform `espressif32` | 7.1.3 |
| `framework-arduinoespressif32` | 4.20017.260907+sha.dcc1105b |
| `tool-esptoolpy` | 2.41100.260830（**esptool v4.11.0**） |
| `toolchain-xtensa-esp32` / `-esp32s3` / `riscv32-esp` | 8.4.0+2021r2-patch5 |
| NimBLE-Arduino | 2.5.1 |
| ArduinoJson | 7.4.3 |

顺手改两处小东西：
- `platformio.ini` 头注释还写着「框架：Arduino-ESP32 3.x」，实际已是 **4.2.17**；
- 发布前清一下陈旧的依赖缓存目录 `firmware/.pio/libdeps/`（里面残留着已删除的 `*-multiuser` 等 env 目录，容易误导）：
  `pio pkg clean -d firmware`。

> 可选省 Flash（只影响 WROOM-32 的余量）：把 `-DCORE_DEBUG_LEVEL=3` 改成 `1`，能省 10–30 KB；
> 当前 4 MB 双槽还剩约 630 KB，**不改也没关系**。

---

## 3. 【发布者】第 1 步：本地构建

VSCode + PlatformIO 插件：左下 Environment 选择器切到目标 env → 勾 Build（√）。
或者命令行（在本仓库根目录）：

```powershell
# Windows PowerShell
pio run -d firmware -e esp32-s3-devkitc-1
pio run -d firmware -e esp32-wroom-32-COM6
```

```bash
# macOS / Linux
pio run -d firmware -e esp32-s3-devkitc-1
pio run -d firmware -e esp32-wroom-32-COM6
```

成功标志（本机实测）：

```
RAM:   [=         ]  17.3% ( 56,596 bytes of 327,680 bytes)
Flash: [===       ]  30.3% ( 1,270,949 bytes of 4,194,304 bytes)   # S3，4MB 视角
=> SUCCESS
```

产物在 `firmware\.pio\build\<env>\`：`bootloader.bin`、`partitions.bin`、`firmware.bin`
（`firmware.bin` = 主固件；`.elf` 只用于调试，不要发到 Release）。

---

## 4. 【发布者】第 2 步：打包两种资产

### 4.1 出厂镜像（合并单文件，推荐做主推资产）

```powershell
# 变量：一次设好
$B    = "firmware\.pio\build\esp32-s3-devkitc-1"
$ESPT = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$PY   = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$A0   = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"

# ESP32-S3（16MB）
& $PY $ESPT --chip esp32s3 merge_bin -o release\wanan-0.3.1-s3-n16r8-merged.bin `
    -fm dio -ff 80m -fs 16MB -t 0x0 `
    0x0  "$B\bootloader.bin"  0x8000 "$B\partitions.bin"  0xe000 $A0  0x10000 "$B\firmware.bin"
```

```powershell
# 经典 ESP32（4MB）：bootloader 传 0x1000，其余相同
$B = "firmware\.pio\build\esp32-wroom-32-COM6"
& $PY $ESPT --chip esp32 merge_bin -o release\wanan-0.3.1-wroom32-4mb-merged.bin `
    -fm dio -ff 40m -fs 4MB -t 0x0 `
    0x1000 "$B\bootloader.bin"  0x8000 "$B\partitions.bin"  0xe000 $A0  0x10000 "$B\firmware.bin"
```

实测结果（直接可比对）：

| 资产 | 大小 | esptool 输出 |
| --- | --- | --- |
| `-s3-n16r8-merged.bin` | **1,336,848 B**（`0x146610`） | `Wrote 0x146610 bytes ... ready to flash to offset 0x0` |
| `-wroom32-4mb-merged.bin` | **1,453,520 B**（`0x162dd0`） | `Wrote 0x162dd0 bytes ... ready to flash to offset 0x0` |

两个合并包都**刷在 `0x0`**（经典 ESP32 的 `0x0–0x1000` 空洞是 FF 填充，不影响启动）。
输出里那句 `SHA digest in image updated` 是 esptool 4.x 给镜像头补 SHA，正常现象。

> 不加 `--fill-flash-size` 时合并包只有「到 app 末尾」那么大（就是上表的数字），别误以为应该等于 16 MB。

### 4.2 升级包 / 全量包（四文件 zip）

```powershell
$B = "firmware\.pio\build\esp32-s3-devkitc-1"
Copy-Item "$B\bootloader.bin","$B\partitions.bin","$B\firmware.bin" release\s3-full\
Copy-Item $A0 release\s3-full\
Compress-Archive -Path release\s3-full\* -DestinationPath release\wanan-0.3.1-s3-n16r8-full.zip -Force
```

zip 里放一张 `FLASH.txt`（或 `manifest` 片段），把 1.1 的偏移表原样抄进去——**别人只会照着你的说明填**。

### 4.3 校验和

```powershell
Get-ChildItem release\*.bin,release\*.zip | ForEach-Object {
  "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $_).Hash.ToLower(), $_.Name
} | Set-Content release\sha256.txt -Encoding ascii
```

---

## 5. 【发布者】第 3 步：发 Release

打 tag（推荐，方便回溯产物对应哪个 commit）：

```powershell
git tag v0.3.1
git push origin v0.3.1
```

然后二选一：

**A. 网页上传（最简单）** — GitHub → Releases → Draft a new release → 选 tag →
「Upload binaries」把 `release\` 下的 4–6 个文件拖进去 → 正文粘贴第 5.1 节的模板 → Publish。

**B. gh 命令行**

```powershell
gh release create v0.3.1 --title "MoonCheck 0.3.1（免编译刷写版）" --notes-file release\notes.md `
  release\wanan-0.3.1-s3-n16r8-merged.bin release\wanan-0.3.1-s3-n16r8-full.zip `
  release\wanan-0.3.1-wroom32-4mb-merged.bin release\wanan-0.3.1-wroom32-4mb-full.zip `
  release\sha256.txt
```

### 5.1 Release 正文模板（复制即用）

```markdown
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
校园网、账号、坐标、打卡时间写进设备，步骤见本文档第 7 节；用户向图文指南见仓库 `docs/quickstart.md`。

### 本次版本
固件 `0.3.1`（单一固件：打卡几个人由账号表决定，`account add` / `account on|off`，表为空则用设备自身账号）
构建环境：PlatformIO Core 6.2.0 / platform espressif32 **7.1.3** / Arduino-ESP32 4.2.17
/ esptool 4.11.0 / NimBLE-Arduino 2.5.1 / ArduinoJson 7.4.3（源码用同样版本可复现，校验见 `sha256.txt`）

### 免责
本固件仅供你在**自己或已知情同意**的同学账号上使用，用于校园服务平台的定位签到。
请自行确认符合学校相关规定；因使用产生的账号风险由使用者自担。
```

---

## 6. 【使用者】网页刷写的完整操作（一步一步）

### 6.1 准备

- 电脑：Windows / macOS / Linux 都行，**浏览器必须是 Chrome 或 Edge**（Firefox/Safari 没有 Web Serial）。
- 驱动：
  - 板子用 **CH340/CH343** 串口芯片 → 需要装 [WCH CH340 驱动](https://www.wch-ic.com/downloads/CH341SER_EXE.html)；
  - 板子是 **原生 USB（ESP32-S3 的 COM 口显示 "USB Serial Device"）** → Win10/11 免驱；
  - CP210x → 需要 [Silicon Labs VCP 驱动](https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers)。
- 线材：必须是**数据线**。很多 Micro-USB/Type-C 线只能充电，表现为「扫不到串口」。
- 供电：刷写时插电脑 USB 即可；日常运行建议 5V ≥ 500 mA。

> 提示：乐鑫页面自己写着，CH340 在 macOS/Windows/Linux 上常被内核 `usbserial` 驱动占用导致 WebUSB 打不开。
> 遇到 `could not claim interface` 时，**改用「串口模式」而不是 WebUSB 模式**（Connect 按钮旁的串口选择里出现
> `COMx` 就是串口模式），或者换一根带 CH343/CP2102 的板子/线。

### 6.2 刷入

1. 打开 <https://espressif.github.io/esptool-js/>
2. Baudrate：`921600`
3. **Connect** → 浏览器弹出「允许访问串口」→ 选中你的 `COMx` / `usbserial-*` / `cdcacm*`
4. 文件表第一行：`Flash Address` 填 **`0x0`**（十六进制，带 `0x`），`File` 选 `-merged.bin`
5. Flash Mode = `keep`，Frequency = `keep`，Size = `detect`
6. **Program** → 看到 `Wrote ... at 0x0,Hash of data verified.` 即成功
7. **Disconnect** → 按一下板子 `RST/RESET` → 板子开始跑固件（BLE 广播名 `WANAN-XXXX`，
   后缀 = 该板 MAC 最后两字节）

### 6.3 只升级固件（保留配置）

文件表**只填一行**：地址 `0x10000`，文件 = `-full.zip` 里的 `firmware.bin`。其余不动。
（前提：分区表没变过。本仓库 4 MB 板从旧表改成双槽 `0x1F0000` 的那一版**必须整片重刷**，见 6.4。）

### 6.4 换了分区表时必须四文件全刷

| 地址 | 文件 |
| --- | --- |
| `0x0`（S3） / `0x1000`（经典 ESP32） | `bootloader.bin` |
| `0x8000` | `partitions.bin` |
| `0xe000` | `boot_app0.bin` |
| `0x10000` | `firmware.bin` |

在网页文件表里填 4 行，然后 Program。nvs 在 `0x9000`，四个文件都不覆盖它 → **配置不丢**。

### 6.5 常见报错对照

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| Connect 后没有串口可选 | 驱动没装 / 线是充电线 | 装驱动、换数据线；设备管理器里应出现 COM 口 |
| `Failed to connect: No serial port selected` | 没点 Connect 就 Program | 先 Connect |
| 一直 `Connecting....___` 卡住 | 板子不在下载模式 | 按住 BOOT → 点 RESET → 松 BOOT → 再 Program |
| `Invalid head of packet (0x08)` | 上电时序/波特率过高 | 波特率改 `115200`；或换 USB 口（别用 USB HUB） |
| `could not claim interface` | 内核占用（CH340 常见） | 用串口号方式而非 WebUSB；或换 CP210x 板的机器 |
| `invalid header magic` / 刷完没广播 | 用了**另一个型号**的 bin | 按 1.1 重新选对应型号的合并包 |
| 刷完配置全没了 | 刷的是合并包（预期行为） | 重做第 7 节配置；以后升级只刷 `0x10000` |

### 6.6 命令行兜底（给不用网页的人）

```powershell
# S3
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" `
  "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" `
  --chip esp32s3 -b 921600 --port COM5 write_flash -z `
  0x0 bootloader.bin  0x8000 partitions.bin  0xe000 boot_app0.bin  0x10000 firmware.bin
```

---

## 7. 【使用者】刷完后的蓝牙配置（8 项，一次做完）

网页工具**只负责刷固件**，配置要走蓝牙（BLE）。这一步无法省掉——密码和指纹必须进设备。

### 7.1 用电脑（推荐）

```powershell
# 1) 装一个依赖（只需 Python 3.10+）
python -m pip install bleak

# 2) 看有哪些板子（多板时用它区分）
python tools\board_id.py
python tools\ble_console.py --list

# 3) 连上设备（单台自动连，多台会让你选）
python tools\ble_console.py --name WANAN-1A2B
```

进去后按顺序敲（**每行一条**）：

```
wifi UESTC-WiFi                          # 开放网络省略密码；普通网络写 wifi <ssid> <pass>
netcheck                                 # 出现 204 才算放行；302/HTML 说明还要网页认证
auth <学号> <密码> <浏览器指纹>            # 指纹见 7.3
loc <纬度> <经度> <地址>                # 必须在宿舍现场实测（取法见 7.2）
sched 22:00 23:30 30                     # 打卡时间 / 窗口截止 / 抖动
auto on                                  # 打开每日自动
notify set url=<你的推送地址> style=0     # 手机通知；Bark 用 0，ntfy 用 1，自建中转用 2
notify test 你好                          # 手机应立刻收到一条「[WANAN-xxxx] 通知」
```

多人（可选）：

```
account add A01 <学号> <密码> <指纹|->             # 最多 6 个；省略坐标沿用设备坐标
account on  A01        /  account off A01        # 单独开关某人（off 不计入「全员成功」）
account list                                     # 查看表（学号打码，不含密码/指纹）
multi probe force                              # 窗口外完整演练（只体检不提交）
```

最后验收（**必做**，一条命令跑完整条链）：

```
status          # 看 wifiConfigured / user / fingerprintSet / schedule / notifyOn
chain 0         # loginPage→登录→进入打卡界面→业务接口 全链路自测，看 canCheckin
probe           # 只读体检：h5Reached / sessionValid / inWindow / 今日状态
```

`chain 0` 里 `appEntry:true` + `canCheckin:true` = 今晚能自动打上。

### 7.2 坐标怎么取（必须在宿舍现场）

打卡点半径 **130 m**。在手机上用浏览器打开
`https://jzsz.uestc.edu.cn/daily-checkin-h5/index.html?appId=294`，
授权定位后从请求里读 `reverse?lat=<纬度>&lng=<经度>`，把那对数字填进 `loc`。
WiFi 定位可能漂几十米，**晚上在宿舍床上取**，不要在教学楼取。

### 7.3 浏览器指纹（只有第一次需要）

这个值 = 服务端「可信设备」标记，固定下来以后登录就不会再要二次验证码。

1. 电脑 Chrome 登录 <https://idas.uestc.edu.cn/authserver>（勾选「记住设备」之类）；
2. F12 → **Application**（Edge 叫「应用程序」）→ 左侧 **Cookies** → 选 `https://idas.uestc.edu.cn`；
3. 找 **`MULTIFACTOR_BROWSER_FINGERPRINT`**，值是 32 位十六进制 → 复制；
4. 填进 `auth <学号> <密码> <这串值>`。

> 该 Cookie 是 HttpOnly，控制台 `document.cookie` 里看不到，必须走 Application 面板。

### 7.4 没有电脑怎么办

安卓手机装 **nRF Connect for Mobile**：扫描 `WANAN-` → Connect → 找 **Nus** 服务，
`6E400003-...` 特征订阅（拿回执），向 `6E400002-...` 写特征发 JSON，例如：

```json
{"id":1,"op":"wifi.connect","ssid":"UESTC-WiFi"}
{"id":2,"op":"auth.set","user":"<学号>","pass":"<密码>","bfp":"<指纹>"}
{"id":3,"op":"loc.set","lat":30.754980,"lng":103.933880,"name":"示例地点"}
{"id":4,"op":"sched.set","checkin_at":"22:00","window_end":"23:30","jitter_s":30,"auto":true}
{"id":5,"op":"notify.set","url":"<推送地址>","style":0,"on":1}
{"id":6,"op":"test.chain","wait_s":0}
```

指令全集见 `docs/ble-protocol.md` §4.4。

---

## 8. 【发布者】可选：让 GitHub Actions 替你构建

想彻底不手动跑，就加一个 workflow（本仓库还没有，第 4 节命令是等价的手动做法）：

```yaml
# .github/workflows/release.yml（示意，未在本仓库实装）
name: Build firmware
on:
  push:
    tags: ["v*"]
jobs:
  build:
    runs-on: ubuntu-latest
    strategy:
      matrix:
        include:
          - { env: esp32-s3-devkitc-1,     chip: esp32s3, fm: dio, ff: 80m, fs: 16MB, boot: "0x0",    tag: s3-n16r8 }
          - { env: esp32-wroom-32-COM6,    chip: esp32,   fm: dio, ff: 40m, fs: 4MB,  boot: "0x1000", tag: wroom32-4mb }
    steps:
      - uses: actions/checkout@v4
      - uses: platformio/platformio-core-action@v1          # 或 pip install platformio==6.2.0
      - run: pio run -d firmware -e ${{ matrix.env }}
      - name: merge image
        run: |
          python -m esptool --chip ${{ matrix.chip }} merge_bin \
            -o wanan-${GITHUB_REF_NAME#v}-${{ matrix.tag }}-merged.bin \
            -fm ${{ matrix.fm }} -ff ${{ matrix.ff }} -fs ${{ matrix.fs }} -t 0x0 \
            ${{ matrix.boot }} firmware/.pio/build/${{ matrix.env }}/bootloader.bin \
            0x8000 firmware/.pio/build/${{ matrix.env }}/partitions.bin \
            0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin \
            0x10000 firmware/.pio/build/${{ matrix.env }}/firmware.bin
      - uses: actions/upload-artifact@v4
        with: { name: ${{ matrix.tag }}, path: "wanan-*.bin" }
```

要点：
- **必须先做第 2 节的版本锁定**，否则 Actions 装到的 platform/库版本和 Release 说明对不上；
- Release 发布需要 `permissions: contents: write` 或用 `softprops/action-gh-release`；
- Actions 出来的 `boot_app0.bin` 路径与本机一致（同一框架包版本），可以先 `ls` 确认再写死。

---

## 9. 发布前检查清单

- [ ] `platformio.ini` 锁版本（platform 7.1.3 + 两个库精确版本）并重新构建成功
- [ ] 两个 env 都构建通过，Flash 占用分别 ~30% / ~68%（方案 B 分区表）
- [ ] 合并包大小对上 1,336,848 / 1,453,520 B（版本没变的话应完全一致）
- [ ] 每个资产都有 SHA256（`sha256.txt`）
- [ ] Release 正文含：型号区分说明、`0x0` vs `0x1000` 偏移表、**合并包清配置警告**、升级只刷 `0x10000` 的说明
- [ ] 正文含 BLE 配置入口链接（`docs/quickstart.md`），别让使用者以为刷完就能自动打卡
- [ ] 没有把 `.pio/`、`capture/`、`config.json`、`*.har`、真实坐标/学号/指纹打进资产或正文
- [ ] 自己先用一块板子走一遍「合并包 → 配置 → `chain 0`」全流程，再发出去

---

## 10. 这套做法的边界（如实说明）

| 能省 | 不能省 |
| --- | --- |
| 使用者的 PlatformIO / 工具链 / 编译等待 | 浏览器取 `MULTIFACTOR_BROWSER_FINGERPRINT`（F2 验证码中继未实装） |
| 源码仓库都不用 clone（只要下载 bin） | 一次蓝牙配置（WiFi + 账号 + 坐标 + 时间），网页工具不做这件事 |
| 升级只刷 `firmware.bin` 一个文件 | 宿舍现场取坐标 |
| —— | 校园网放行（`netcheck` 必须 204；若某接入点要求 Portal，设备过不了） |

另外两条硬约束别忘了写进任何对外说明：
- **登录只在 21:00–23:30 窗口内进行**（会话 TTL ≈ 1 小时 + 平台只接受窗口内提交）；
- 微应用授权的真开关是门户预热 `GET /api/isLoginFailure`（见 `docs/api-contract.md` §8.7），
  固件已内置，自行改请求路径的人会把这层丢掉。
