# 🌙 MoonCheck

**ESP32-S3 nightly location check-in bot · 基于 ESP32-S3 的宿舍晚间自动定位打卡终端**

> 手机蓝牙配置一次，设备每晚自动完成「登录 → 门户预热 → 进入微应用 → 定位打卡 → 结果确认」。
> Moon = 晚安（Night），Check = 打卡（Check-in）。
>
> *Provision once over Bluetooth Low Energy; the device handles the rest, every night, unattended.*

```
┌──────────────┐   BLE 配网    ┌───────────────────────┐   HTTPS/TLS   ┌──────────────────────┐
│  手机/电脑   │ ───────────▶ │  ESP32-S3 · MoonCheck │ ────────────▶ │  学校平台（CAS + 打卡）│
│  nRF Connect │ ◀─────────── │  21:00 预登录 22:00 打 │ ◀──────────── │  统一身份认证 / 学工  │
└──────────────┘   状态/结果   └───────────────────────┘    结果确认    └──────────────────────┘
```

---

## ⚠️ 免责声明（请先读）

- 本项目用于**设备持有者本人账号**的自动打卡。仓库另提供一个**多人顺序打卡版**（一台设备为同一宿舍内**互相知情的几个账号**依次登录+打卡）——使用它必须获得每一位账号持有者的明确授权，**不做代打服务、不对外分发、不用于营利**；
- 自动化打卡**可能不符合学校规定**，请自行评估并承担后果；
- 请勿用于绕过任何服务端安全校验、请勿高频请求（本项目默认每天只登录一次、只打卡一次）；
- 设备内会保存学号与密码（NVS 明文，可选开启 Flash 加密），**请勿转让/借出设备**，丢失后立即改密；
- 本项目按 MIT 协议开源，**作者不对使用后果承担任何责任**。

---

## 1. 它能做什么

| 能力 | 说明 |
| --- | --- |
| 🔧 蓝牙配网 | 手机装 nRF Connect 即可完成全部配置，**无需自研 App**（也可用仓库里的 PC 端调试台） |
| 🔐 一次配置长期有效 | 首次配置好账号密码 + 「信任浏览器」指纹后，**不再需要二次验证码** |
| ⏰ 每晚自动打卡 | **21:00（打卡窗口一开始）**自动预登录体检，22:00（含随机抖动）提交打卡，失败自动重试至窗口结束。登录后固件会自动做一次**门户预热**（`GET /api/isLoginFailure`）——这才是微应用授权的前提；预登录放在窗口起点是因为会话 TTL 仅约 1 小时 |
| ✅ 结果自证 | 打卡后主动复查状态确认，并把结果写入 NVS，**掉电重启也不会丢** |
| 🧪 随时可测 | 提供 `test.http` / `test.login` / `test.chain` / `test.probe` 等只读指令，**非打卡时间也能验证前置链路**（`chain` 把"能登录"和"能进打卡界面"分开报告） |
| 🔌 掉电自恢复 | 配置、会话、上次结果全部持久化；重启后自动重连 WiFi、校时、续跑调度 |
| 🩺 自带诊断 | `chain`（全链路逐段判定）、`netcheck`（是否需要网页认证）、`scan`（看有没有 2.4 GHz）、`logs`（设备日志回读），排障不靠猜 |
| 🏷 可自定义设备名 | `name A01` → 广播名变 `WANAN-A01`（多块板时按名字认领，存 NVS、在线生效） |
| 🧩 多人顺序打卡版 | 一台设备最多 6 个账号，依次"登录 → 门户预热 → 打卡 → 复查"，每人独立幂等、失败互不影响（见 `docs/multi-user-design.md`） |
| 🖥 多构建目标 | 同一份代码支持 **ESP32-S3**（16 MB / 无 PSRAM 均可）与**经典 ESP32-WROOM-32**，LED 与 PSRAM 自动适配 |

---

## 2. 工作原理

完整链路共 5 步（与浏览器手动打卡完全一致）：

```
①  CAS 登录        取登录页 → AES 加密密码 → 提交 → 302 带票据
②  票据换会话      跟票据 → 拿到平台 SESSION
②.5 门户预热       GET /api/isLoginFailure  ← 微应用授权的前提，缺了它 ③ 必被退回首页
③  进入微应用      /daily-checkin/checkLogin?appId=xxx → H5 页
④  定位打卡        POST 打卡接口（经纬度 + 时间）→ 复查状态确认
```

**其中有两个平台特有的机制，是本项目能跑通的关键（也是踩坑最多的地方）：**

| 机制 | 说明 |
| --- | --- |
| **① 免二次验证靠"浏览器指纹"** | 服务端把 `(账号, MULTIFACTOR_BROWSER_FINGERPRINT)` 记为可信设备。**设备只要固定使用抓包得到的那个指纹值，就等价于那台被信任的浏览器**，从此不再索要验证码。 |
| **② 微应用要求"门户预热"** | CAS 登录成功后，会话还必须被门户初始化一次：`GET /api/isLoginFailure`。**没做这一步**时登录看似成功、平台首页也能打开，但打卡微应用一律拒绝（`checkLogin` 被 302 退回首页、接口返回 `4001 登录无效或已过期`）；补上这一个 71 字节的请求，**同一会话立刻可用**（2026-10-13 实测）。 |

> 第 ② 条意味着：**每次登录之后必须紧跟一次门户预热**（固件 `CheckinApi::warmUpPortal()` 已内置，并在 `probe`/`chain` 里做自愈）。
> 另外 `SESSION` 的 TTL **只有约 1 小时**，所以本项目把预登录定在**窗口一开始的 21:00**（既贴近打卡时间，又留出重试余量），发现会话失效时**当场补登录 + 补预热**。

---

## 3. 硬件与软件准备

### 3.1 硬件

| 项目 | 要求 |
| --- | --- |
| 主控 | ESP32-S3 开发板，**建议 N16R8（16 MB Flash + 8 MB PSRAM）**；N8 无 PSRAM 也能跑 |
| 供电 | USB-C 5 V 常供电（宿舍插座 + 防拔固定） |
| 交互 | 无需任何外设：板载 LED（默认 WS2812 @ GPIO48）+ BOOT 按键即可 |
| 网络 | **2.4 GHz WiFi**（ESP32-S3 不支持 5 GHz）。校园网若为 802.1X/Portal 需自行改造（见 §9） |

### 3.2 软件

| 用途 | 工具 |
| --- | --- |
| **只配网、不改代码** | Python **3.12** + `pip install bleak` → 用 `tools/ble_console.py`（单文件，够用） |
| 抓包取参数（可选） | Burp Suite Community + Chrome/Edge DevTools（见 `docs/capture-guide.md`） |
| PC 端先验证链路 | Python 3.10+，`pycryptodome`（诊断脚本另需 `bleak`） |
| 编译烧录 | VSCode + **PlatformIO IDE 插件**（首次会下载 espressif32 平台与工具链，约 1–2 GB；经典 ESP32 还需另一套 `toolchain-xtensa-esp32`） |

> 🙋 **拿到板子只想配好就走？** 直接看 **`docs/quickstart.md`**（同学上手指南，含配图位与逐步命令）。

---

## 4. 快速开始（TL;DR）

```powershell
# 0) 只想配好就走 → 看 docs/quickstart.md（逐步命令 + 排错）
# 1) 取 3 个值（不用抓包）：浏览器指纹、宿舍坐标、学号密码        → §4.1
# 2)（可选，强烈建议）PC 端先把链路跑通，排除 90% 风险
cd tools\pc_reference
copy config.example.json config.json     # 填学号/密码/指纹/坐标
python verify.py selftest
python verify.py status                  # 只读：能读到今日状态就说明链路通
# 3) 烧固件：VSCode 打开 firmware\ → 选环境（S3 单人 / S3 多人 / 经典 ESP32）→ Build → Upload
# 4) 蓝牙配网（二选一）
python ..\tools\ble_console.py --list     # 看板子在不在（广播名 WANAN-XXXX）
python ..\tools\ble_console.py --name WANAN-1A2B
#   或手机 nRF Connect 连接同名设备
# 5) 验证链路 → 放着不管
> chain 0        # 期望 appEntry:true（warmup:true）；inWindow 白天为 false 属正常
> checkin        # 21:00–23:30 内可直接打一次
```

---

## 4.1 免抓包快速启动（不需要 Burp Suite）★ 推荐先试

**结论：绝大多数情况你并不需要抓包。** 只要你能在浏览器里打开打卡页面，就只需要抄 **3 个值**：

| # | 值 | 从哪抄（Chrome / Edge 都适用） |
| --- | --- | --- |
| ① | 学号 / 密码 | 你自己知道（**密码不必抓**：设备侧自己做 AES 加密后提交） |
| ② | **浏览器指纹** | `F12` → **Application（应用程序）** 面板 → 左侧 **Cookies**（新版 Chrome/Edge 在 `Storage → Cookies` 下）→ 选 `https://idas.uestc.edu.cn` → 找 **`MULTIFACTOR_BROWSER_FINGERPRINT`** 这一行 → 复制 **Value**（32 位十六进制） |
| ③ | **打卡坐标** | 在浏览器里打开「晚安成电」页面 → `F12` → **Network** → 过滤框输入 `reverse` → 点 `map/reverse?lat=…&lng=…` 那一行 → 从 URL 里读出 `lat` / `lng` |

**为什么只要这两个值就够**

- **指纹**就是"免二次验证"的全部秘密：服务端把 `(账号, 指纹值)` 记为可信设备，设备固定用同一个值就等价于你那台被信任的浏览器（原理见 §2 与 `docs/api-contract.md` §8.3）；
- **坐标**只需抄一次：设备常驻宿舍，坐标固定；
- **SESSION 不需要你提供**：设备会**自己完成 CAS 登录 + 门户预热**并拿到可用会话（已实测：设备自行登录后成功打卡，根因与判据见 `docs/api-contract.md` **§8.7**）。

⚠️ **三个注意点**

1. 指纹 Cookie 是 **`HttpOnly`**，所以在 Console 里执行 `document.cookie` **看不到它** —— 必须用 **Application → Cookies**；
   等价办法：在 Network 里找 `GET /authserver/bfp/info?bfp=…` 请求，URL 里 `bfp=` 后面就是它（明文可读）。
2. 指纹必须从**已被信任的浏览器**里抄（即你平时打卡、登录时勾过"信任该浏览器"的那个）。从没验证过的浏览器里抄出来的值服务端不认，设备仍会被要求验证码。
3. 指纹**每个浏览器配置独有**：换浏览器或清空站点数据后要重新抄一次。

**完整启动流程（不需要 Burp / HAR / 会话注入）**

```powershell
python -m pip install bleak
cd tools
python ble_console.py            # 自动扫描 WANAN-* 并连接
> wifi <宿舍WiFi名> <WiFi密码>
> auth <学号> <密码> <②指纹>
> loc <③lat> <③lng> 四川省成都市郫都区
> sched 22:00 23:30 30           # 可选：确认打卡时间与窗口
> probe                          # 只读体检：能看到今日状态 / 时段 / 打卡点半径
> chain 0                        # 全链路自检：能否登录 + 能否进入打卡界面（见 Step 5）
```

手机党用 nRF Connect 发同样的 JSON 指令即可（见 Step 4）。

**然后就不用管了**：设备 21:00（窗口一开始）自动预登录、22:00 自动打卡；**每次登录后都会自动补一次门户预热**（这是微应用授权的前提，见 §2 机制 ②），失败则每 3 分钟重试到 23:30。

> 💡 配网前先输 `scan` 确认你的 WiFi 在 **2.4 GHz**（`channel` 1–13）；校园网环境再输一次 `netcheck`，
> 返回 `204` 说明不需要网页认证，能直接给设备用。

**什么时候才真的需要 Burp？**

| 场景 | 需要抓包吗 |
| --- | --- |
| 只想让设备跑起来（本校、接口未变） | ❌ **不需要**，按本节即可 |
| 想弄清接口字段、判断是否存在签名（二次开发） | ✅ 需要，见 `docs/capture-guide.md` |
| `4001` 之类问题反复出现，要看原始报文 | ✅ 需要 |
| 移植到**其他学校**（CAS 与打卡接口都不同） | ✅ **必须**重新抓包 |

---

## 5. 复刻详细步骤

### Step 0 · 先搞清你要收集哪些信息

复刻的本质是**把浏览器里手动做的五步（登录 → 换会话 → 门户预热 → 进微应用 → 打卡），翻译成设备上的 HTTP 请求**。其中"门户预热"是最容易漏、也最致命的一步。需要从中提取：

| # | 参数 | 从哪里得到 | 备注 |
| --- | --- | --- | --- |
| 1 | 学号 / 密码 | 你自己知道 | 密码会被 AES 加密后提交，设备里存的是明文（NVS） |
| 2 | **浏览器指纹** | DevTools → **Application → Cookies** → `idas.uestc.edu.cn` → `MULTIFACTOR_BROWSER_FINGERPRINT`（32 位 HEX） | **最关键**：决定是否还会要验证码。见 §4.1，**不需要抓包** |
| 3 | **打卡坐标** | DevTools → **Network** → `map/reverse?lat=…&lng=…` 的 URL（或一次成功打卡请求里的 `latitude`/`longitude`） | 用你**实际打卡时所在位置**的坐标（宿舍），不是伪造 |
| 4 | 打卡时段 | 接口返回的 `studentCheckin*OffsetMinute`（分钟偏移） | 决定设备何时动手 |
| 5 | 宿舍 WiFi SSID/密码 | 你自己知道 | 必须是 2.4 GHz |

> ⚠️ **非本校学生**：CAS 登录页、二次验证、打卡接口均为学校私有实现，你需要重新完成 Step 1 抓包，并按 `docs/api-contract.md` 的格式改 `firmware/src/checkin_api.cpp` 与 `cas_auth.cpp`。设计文档里的**分层与适配层隔离**就是为此准备的——理想情况下只需改这一个文件。

### Step 1 · 抓包（**仅在你需要时才做**，约 30 分钟）

> 💡 **先看 §4.1**：如果只是想让设备跑起来，**你不需要抓包**——指纹从 DevTools 的 Cookie 面板抄、坐标从 `map/reverse` 请求的 URL 抄即可。
> 本节面向这些场景：想彻底搞清接口字段 / 判断是否存在签名 / 移植到其他学校 / 排查 `4001` 反复失败。

按 **`docs/capture-guide.md`** 逐步执行。这个指南本身就是为"没抓过包的人"写的，包含：

- Burp CA 证书安装到 Windows 根存储、关闭 QUIC、独立浏览器配置等环境准备；
- 分幕操作（登录 → 二次验证 → 进平台 → 点打卡图标 → 提交）；
- **关键坑**：点击打卡图标会打开**新标签页**，DevTools 只记录自己所在标签页 → 必须以后台 Burp 记录为准，并给新标签页单独开 F12；
- 需要记录的字段清单，以及**自检勾选项**。

抓完你会得到：`#2 登录请求`、`#4 票据`、`#7 打卡请求` 等原始报文，从中抄出上表的 5 个参数。

> 🔒 **抓包文件含有你的密码、验证码、以及等价于账号通行证的 Cookie。切勿上传到任何仓库或发给他人。** 仓库的 `.gitignore` 已默认忽略 `capture/`。

### Step 2 · PC 端先验证（约 1 小时，**强烈建议**）

**不要一上来就写固件。** 先用 Python 在电脑上把链路跑通，因为电脑上能看到完整的请求/响应，出问题好排查。

```powershell
cd tools\pc_reference
copy config.example.json config.json
notepad config.json          # 填学号/密码/指纹/坐标
python -m pip install pycryptodome
python verify.py selftest    # 离线自检：验证 AES 加密实现与配置完整性
python verify.py status      # 只读：读今日打卡状态（零风险）
```

详细的分阶段验证方案（V0–V5）见 **`docs/python-verification-plan.md`**，核心是四件事：

| 验证 | 命令 | 通过意味着 |
| --- | --- | --- |
| V1 打卡接口可脚本化 | `python verify.py status` | 接口无签名/无风控，**项目可行** |
| V2 登录可脱离浏览器 | `python verify.py login` | 密码加密算法与票据流可复现 |
| V3 免二次验证条件 | `python verify.py login --bfp random` | 随机指纹会被要求验证码 → 证明**必须固定指纹** |
| V4 会话寿命 | 每天 `python verify.py probe-session` | 决定设备需要多频繁登录 |

> 💡 `verify.py checkin` 会先查状态再打卡（幂等），且自带 `--dry-run`。**一天只有一次真实打卡机会，别拿它做实验。**

### Step 3 · 烧录固件

1. VSCode 打开 **`firmware/` 文件夹**（不是仓库根目录），PlatformIO 插件会自动识别工程；
2. 首次 Build 会下载 espressif32 平台与工具链（1–2 GB；经典 ESP32 另需 `toolchain-xtensa-esp32`）；
3. **选构建环境**（左侧 PlatformIO → Environment 选择器）：

   | env | 用途 |
   | --- | --- |
   | `esp32-s3-devkitc-1` | **单人版**（默认），ESP32-S3-N16R8 |
   | `esp32-s3-multiuser` | **多人顺序打卡版**（同一块 S3，只多 `-DWANAN_MULTIUSER=1`；`multi.set` 可随时关掉退回单人行为） |
   | `esp32-wroom-32-a` / `-b` | 经典 ESP32-WROOM-32（4 MB / 无 PSRAM），两块分别绑不同 COM 口 |

4. **Build** → **Upload** → **Monitor**（115200）；多块板同时插着时**务必写死端口**或加 `--upload-port COMx`，避免刷错板；
5. 串口默认自动探测；识别不到时在 `firmware/platformio.ini` 对应 env 里填 `upload_port` / `monitor_port`。

**上电后应有的日志**：

```
=== WANAN checkin device booting ===
psramInit(): PSRAM enabled            ← 有 PSRAM 的板子会出现
logger ready (ring=ok)
ble ready: name=WANAN-XXXX (service 6E400001-...)
setup done, free heap=219048 B, psram=8357183 B
wifi configured -> connecting in background   ← 配好网后才有
```

| 常见坑 | 处理 |
| --- | --- |
| `Failed to connect: Invalid head of packet` | 按住 **BOOT** → 点 **RST** → 松 BOOT，再点 Upload（进下载模式） |
| 串口无输出 | 板子有两个 USB-C 口时换另一个；并把 `-DARDUINO_USB_CDC_ON_BOOT` 在 `1`/`0` 之间对调 |
| `Partitions overlap` | 检查 `firmware/partitions/partitions_16mb.csv` 的偏移（nvs 0x5000 → otadata 0xE000 → app0 0x10000） |
| `UnicodeDecodeError: 'gbk' codec ...` | 分区表 CSV **必须保持纯 ASCII**（PlatformIO 用系统编码读它） |
| 灯不亮/颜色不对 | 改 `firmware/include/app_types.h` 的 `kLedPin`（默认 48）与 `kLedIsRgb` |

### Step 4 · 蓝牙配网

**方式 A（推荐）· PC 端调试台**

```powershell
python -m pip install bleak
cd tools
python ble_console.py --list            # 看广播名（WANAN-XXXX = 蓝牙 MAC 末两字节）
python ble_console.py --name WANAN-1A2B # 多块板时按名字连，避免连错
> hello                               # 看固件版本、MAC、Flash/PSRAM 实测值
> scan                                # 你的 WiFi 在不在 2.4 GHz（channel 1–13）
> netcheck                            # 校园网是否需要网页认证（204=不需要）
> wifi 你的WiFi名 你的WiFi密码
> auth 学号 密码 抓包得到的32位指纹
> loc 30.75498 103.933883 四川省成都市郫都区      # ★ 换成你自己宿舍的实测坐标
> sched 22:00 23:30 30                # 打卡时间 / 窗口结束 / 抖动
> auto on
> name A01                            # 可选：给这块板起个好认的名字
> status
```

**方式 B · 手机 nRF Connect**：连接 `WANAN-XXXX` → 打开 Nordic UART Service → 订阅 TX（Notify）→ 向 RX 写入同样的 JSON 指令（每条以换行结尾）：

```jsonc
{"op":"wifi.set","id":2,"ssid":"宿舍WiFi","pass":"密码"}
{"op":"auth.set","id":3,"user":"2023XXXXXXXXX","pass":"密码","bfp":"抓包得到的32位指纹"}
{"op":"loc.set","id":4,"lat":30.75498,"lng":103.933883,"addr":"你的地址文本"}   // ★ 换成你自己的实测坐标
{"op":"sched.set","id":5,"at":"22:00","window_end":"23:30","jitter_s":30}
```

### Step 5 · 自测（**非打卡时间也能做**）

这是本项目刻意设计的能力：把"能不能上网、能不能登录、能不能进打卡页"与"能不能打卡"分开验证。

| 指令 | 验证内容 | 何时可跑 |
| --- | --- | --- |
| `hello` | 固件版本 / Flash / PSRAM / 能力位 | 随时 |
| `http <登录页URL>` | DNS + **TLS 证书校验** + HTTP | 随时 |
| `time` | SNTP 校时是否准确、是否在窗口内 | 随时 |
| `login [force]` | **完整 CAS 登录链**（取盐 → AES 加密 → 提交 → 换 SESSION）。默认**窗口外不登录**（会话 TTL 约 1 小时，窗口外登录会白白过期），`login force` 才强制 | 随时（默认受窗口保护） |
| `chain [wait_s] [force]` | **全链路自检**：登录页 → 登录 → **进入打卡界面** → 业务接口。`wait_s` 秒内每 30 s 重探一次放行（上限 900） | 随时（`canCheckin` 仅窗口内为 true） |
| `probe` | **前置体检**：会话 / 今日状态 / 平台时段 / 打卡点半径 | 随时 |
| `scan` | 扫描周边 AP（设备只看得见 2.4 GHz） | 随时 |
| `netcheck` | 当前网络是否需要网页认证（204=不需要） | 随时 |
| `logs 30` | 回读设备日志（排障主力） | 随时 |
| `checkin` | 真正打卡 | **仅 21:00–23:30** |
| `account …` / `multi …` | **仅多人版**：管理账号表、按顺序跑一轮 | 随时（提交需窗口内） |

`probe` 的期望输出：

```json
{"sessionValid":true,"h5Reached":true,"window":"21:00 - 23:30","inWindow":false,
 "windowFromPlatform":true,"recordStatusCode":"PENDING",
 "location":"本科3组团","radiusMeter":130}
```

> `h5Reached` 是判定"微应用是否放行"的探针：`false` 通常意味着**这个会话没做过门户预热**（缺 `GET /api/isLoginFailure`，见 §2 机制 ②）。

`chain` 的期望输出（**把"能进界面"和"能打卡"分开看**）：

```json
{"loginPage":true,"session":true,"h5Page":true,"didLogin":false,"loginDeferred":false,
 "appEntry":true,"entryResult":"Ok","entryStatus":200,"entryAttempts":1,"waitedS":0,
 "today":"PENDING","window":"21:00 - 23:30","inWindow":false,"canCheckin":false}
```

| 字段 | 为 false 说明什么 |
| --- | --- |
| `loginPage` | 登录页取不到或结构变了（TLS / 平台改版） |
| `session` | 登录链没成功；配合 `loginDeferred` 看是否因窗口外被推迟 |
| **`h5Page`** | **打卡界面（静态 H5 页）打不开** → 网络或平台故障（浏览器也打不开） |
| **`appEntry`** | **平台没授权这个会话打卡**（`checkLogin` 被 302 退回平台首页） |
| `entryStatus` / `entryUrl` | EP05 的最终状态码与最终 URL，一眼看出 302 到底去了哪 |
| `inWindow` | 不在 21:00–23:30，此时 `canCheckin:false` **是正常的** |
| `canCheckin` | 只有 `appEntry && session && inWindow` 三者齐备才为 true，为 true 时按 `checkin` 必成 |

> **"能进界面" ≠ "能打卡"（实测结论，务必分清）**
> 打卡界面是**静态页**：`GET /daily-checkin-h5/index.html?appId=294` **任何时间都返回 200**——所以浏览器里"任意时间输入晚安成电网址都能进去"是真的；
> 但**真正决定能不能打卡**的是平台对会话的授权，判据是 `checkLogin` 是否 302 到 H5 页（＝`appEntry`）。
> 2026-10-13 全天实验（`diag_entry.py` → `diag_portal_replay.py` → `diag_minimal_warmup.py`）：一开始以为是"窗口外才被拒"，二分后锁定真正原因——**会话缺少一次门户预热**；补 `GET /api/isLoginFailure` 后同一会话立刻 `checkLogin`→H5、`today` code=0。
> **结论：微应用授权的前提是"门户预热"，与窗口无关；窗口只决定能不能提交打卡。**
>
> 会话 TTL 约 **1 小时**，所以固件把预登录放在**窗口起点 21:00**（贴近 22:00 又留足重试余量），窗口内每 3 分钟重试；不要手动反复 `login`（30 分钟冷却）。
> 想亲眼看整条链：`chain 0`（会依次报告 登录页 / 会话 / h5Page / **warmup** / appEntry / canCheckin）。

### Step 6 · 定时打卡与验收

配好后就**不用管了**。设备会在：

- **21:00（窗口一开始）** 自动预登录体检（若会话失效，**当场在窗口内补登录**）；
- **22:00 ± 0–30 s** 自动打卡，失败每 3 分钟重试，直到窗口结束（默认 23:30）。

**验收标准**（次日看即可）：

```
> status
"last": {"date":"2026-10-13","ok":true,"message":"checked in at 21:36:01 (distance 42.5 m)","ts":...}
> probe
"recordStatusCode": "CHECKED_IN"
```

### Step 7 · 掉电与长稳验证

| 测试 | 操作 | 期望 |
| --- | --- | --- |
| 掉电恢复 | 直接拔电再插上，等 15 s 后 `status` / `probe` | 配置、Cookie、上次结果都在；`probe` 仍为 `sessionValid:true` |
| 窗口内掉电 | 21:00–23:30 之间断电重启 | 自动重新触发打卡；若已打卡则返回 `AlreadyDone`，**不会重复** |
| 幂等 | 同一天连续多次 `checkin` | 平台侧只有一条记录 |
| 长稳 | 连续运行 14 天 | 无需人工干预，`status` 每天 `ok:true` |
| **登录后立刻失败** | 窗口内登录成功后马上 `probe` → `h5Reached:false` | **根因是"没做门户预热"**（已在 `CheckinApi::warmUpPortal()` 修复，并在 `probe` 里自愈）。固件会在窗口内每 3 分钟自动重试，**不要手动反复 `login`**（30 分钟内重复登录被冷却拒绝） |
| **BLE 连不上 / `Could not get GATT services: Unreachable` / 服务发现超时** | **Windows 侧**（与固件无关）：内存里的旧连接状态、或蓝牙射频太弱 | ① 关闭再打开蓝牙开关（或管理员 `Restart-Service bthserv`）；② 用 `tools\clear_ble_cache.ps1` 查看/删除缓存的设备节点；③ **最省事是用手机 nRF Connect**——手机可贴近设备、且没有 Windows 的 GATT 缓存问题；④ 台式机若用 Intel 无线网卡，**确认机箱后天线已拧上**（漏拧是"蓝牙/WiFi 都很弱"的典型原因） |

---

## 6. 故障排查速查

| 现象 | 大概率原因 | 处理 |
| --- | --- | --- |
| `today business code=4001 msg=登录无效或已过期` | **会话没做门户预热**（缺 `GET /api/isLoginFailure`；§2 机制 ②） | 在 21:00–23:30 内跑 `login`，或直接等 21:00（窗口一开始）的自动预登录补登录 |
| `checkLogin` 被 302 退回平台首页 | 同上（`h5Reached:false` 是它的探针） | 同上；跑 `chain 0` 看回执的 `warmup` / `warmStatus` 字段，**别反复 `login`** |
| `NEED_MFA` / 二次验证页 | 指纹未配置或不匹配 | `auth` 重新写入抓包得到的指纹；`login --bfp random` 可作为对照实验 |
| `test.http` 返回 `NETWORK_ERROR` | WiFi / DNS / TLS | 确认 2.4 GHz、能 ping 通外网；若证书链变了，用 `tools/gen_ca_pem.js` 重新生成 `firmware/include/uestc_ca_pem.h` |
| `psramKB: 0` | 板级配置没启用 PSRAM | 确认 `platformio.ini` 的 `board_build.arduino.memory_type = qio_opi` 与 `-DBOARD_HAS_PSRAM`；**不影响功能**（空闲堆 ~200 KB 足够） |
| BLE 连上但服务发现超时 | Windows 侧 GATT 缓存/旧连接残留 | 先烧新固件并断电重启；手机关掉 nRF Connect；Windows 删除设备后重开蓝牙 |
| `E NimBLEAttValue: val > max` | 长回执分片尺寸超限 | 已在 v0.1 修复（base64 分片 + MTU 变化监听）；旧固件请升级 |

---

## 7. 需要你自己填写的参数

| 参数 | 示例 | 说明 |
| --- | --- | --- |
| WiFi SSID / 密码 | `Dorm-2.4G` / `******` | **必须 2.4 GHz** |
| 学号 / 密码 | `2023XXXXXXXXX` / `******` | 统一身份认证凭据 |
| 浏览器指纹 | `<32 位大写 HEX>` | 抓包中 `MULTIFACTOR_BROWSER_FINGERPRINT` 的值 |
| 打卡坐标 | `30.75498, 103.933883` | **必须替换为你宿舍的实测坐标**（抓包中那次成功打卡请求里的经纬度）。表内示例是平台公布的**打卡点中心**（公开围栏数据，距离 0 m），仅保证开箱可运行 |
| 地址文本 | `四川省成都市郫都区` | 打卡请求里的 `reverseGeocode`（示例值），可留空或由接口刷新 |
| 打卡时间 / 窗口结束 | `22:00` / `23:30` | 依据平台实际时段（接口可查，见 `probe`） |

---

## 8. 目录结构

```
mooncheck/
├─ README.md                      ← 你正在看的这份
├─ LICENSE                        MIT
├─ .gitignore                     默认忽略 capture/、*.har、config.json、out/（含凭据）
├─ docs/                          设计文档（中文，逐篇递进）
│  ├─ quickstart.md               ★ 同学上手指南（只要配好就走，含配图占位）
│  ├─ capture-guide.md            抓包操作指南（含自检清单）
│  ├─ python-verification-plan.md PC 端验证方案 V0–V5
│  ├─ python-test-design.md       Python 参考实现的模块与接口设计
│  ├─ api-contract.md             接口契约（唯一真相来源；§8.6 排除清单、**§8.7 根因**）
│  ├─ firmware-design.md          固件设计：16 个模块的功能与接口
│  ├─ multi-user-design.md        多人顺序打卡版设计（账号表、编排、风险）
│  ├─ ble-protocol.md             蓝牙指令/事件协议（含"已实装指令对照表"）
│  ├─ network-uestc-wifi.md       校园网接入判定（802.1X / Portal / 2.4 GHz）
│  ├─ test-plan-checkin-path.md   排查方法记录：分层验证 + 二分定位根因
│  └─ assets/quickstart/          上手指南配图存放处（含打码提醒）
├─ firmware/                      固件（PlatformIO + Arduino-ESP32 3.x）
│  ├─ platformio.ini              多构建目标：S3 单人 / S3 多人 / 经典 ESP32
│  ├─ platformio.wroom32.ini.example  经典 ESP32 环境模板（避免插件解析依赖时卡住）
│  ├─ partitions/                 partitions_16mb.csv / partitions_4mb.csv（纯 ASCII）
│  ├─ include/                    app_types.h + 16 个模块头 + 内置 CA 证书链
│  ├─ src/                        与头文件一一对应的 .cpp + main.cpp
│  └─ README.md                   固件使用说明（环境切换、LED、PSRAM 降级）
└─ tools/
   ├─ ble_console.py              PC 端蓝牙调试台（配网 + 全部自测 + 分片重组 + 选设备）
   ├─ board_id.py                 串口 ↔ 板子对照表（MAC / 芯片 / Flash / 广播名）
   ├─ clear_ble_cache.ps1         清理 Windows 的 BLE GATT 缓存
   ├─ pc_reference/               Python 参考实现 + 诊断脚本（先验证再写固件）
   │   verify.py / wsc_*.py / recon_mfa.py
   │   diag_entry.py        入口对照：极简头 vs 浏览器头 vs 直接访问 H5
   │   diag_session.py      会话来源对照（是否需预注入 SESSION）
   │   diag_existing.py     用已保存会话零登录复测（判断授权跟谁绑定）
   │   diag_bfp.py          补 bfp/info 预登记是否解锁授权（结论：不解锁）
   │   diag_activation.py   新会话轮询 14 分钟，测"激活延迟"（结论：无延迟）
   │   diag_referer.py      Referer 变体矩阵（结论：无关）
   │   diag_portal_replay.py 分层回放门户 XHR（★ 在这一步发现根因）
   │   diag_minimal_warmup.py 二分定位最小触发集（★ /api/isLoginFailure）
   │   diag_ttl.py          SESSION 生命周期测量
   │   companion_probe.py   自动化浏览器登录可行性（备用方案）
   │   diag_impersonate.py  Chrome TLS 指纹伪装对照（备用方案）
   ├─ extract_burp.js             Burp XML → 请求清单 + Set-Cookie 清单
   ├─ analyze_har.py              脱敏查看 Chrome HAR（只看结构，不打印 Cookie 值）
   ├─ extract_portal_sequence.py  HAR → 可回放的门户请求序列
   ├─ inspect_app_js.py           从 HAR 的 JS 里挖"点图标"的代码路径
   ├─ compare_cookies.py          各主机 Cookie 名称对照（值全部隐藏）
   ├─ extract_app_urls.py         从 HAR 里挖应用启动地址
   ├─ show_capture_item.py        脱敏查看单条抓包条目
   ├─ probe_cas_crypto.js         从密文反推密码加密算法（逆向必备）
   ├─ gen_ca_pem.js               抓真实证书链 → 生成固件 CA 头文件
   ├─ check_tls.js                检查证书链与 TLS 版本
   ├─ fetch_asset.js              抓取公开静态资源
   └─ sync_to_mooncheck.py        维护者用：同步到公开副本（自动跳过脱敏文件）
```

---

## 9. 开发进度

| 里程碑 | 内容 | 状态 |
| --- | --- | --- |
| M0 接口契约 | 抓包还原 10 个端点、密码算法（源码+密文双确认）、载荷模板、围栏半径、打卡窗口 | ✅ `docs/api-contract.md` v1.4 |
| M1 PC 参考实现 | Python 独立跑通"登录 → 换会话 → 进微应用 → 打卡"，证明无签名/无风控 | ✅ `tools/pc_reference/` |
| M2 固件链路 | 16 个模块、BLE 配网、NVS 持久化、TLS 校验、幂等与重试、结果自证 | ✅ 已上板验证 |
| **M3 端到端自动打卡** | 设备**自行登录 + 门户预热 + 提交 + 复查**全链路成功（`CHECKED_IN`，`distance` 在 130 m 内，`abnormalFlag=false`） | ✅ 2026-10-13 实测 |
| M4 根因定位 | 用"分层回放 + 二分"锁定微应用授权的唯一前提 = 门户预热 `GET /api/isLoginFailure`，并推翻此前两条误判 | ✅ §8.7 |
| M5 多板与运维 | `name.set` 自定义设备名、`board_id.py` 串口↔板子对照、多构建目标（S3 / 经典 ESP32 / 多人版）、`scan`/`netcheck` 自检 | ✅ 代码完成 |
| M6 多人顺序打卡 | 一台设备最多 6 个账号依次登录+打卡，每人独立幂等与结果记录 | ✅ 代码完成，⏳ 待多账号实测 |
| M7 长稳验证 | 连续 14 天无人值守、掉电自恢复、窗口内重试收敛 | ⏳ 进行中 |

## 10. 已知限制与 Roadmap

| 限制 | 说明 / 计划 |
| --- | --- |
| 日志仅在内存（128 行环形缓冲） | 掉电会丢失；`status` 里的 `last` 字段已持久化结果。计划加 flash 日志 |
| 二次验证码中继未实现（F2） | 若服务端某天清除了指纹信任，设备会报 `NEED_MFA`；计划接入蓝牙回传验证码 |
| 会话 TTL 约 1 小时 | 所以设备每天在窗口起点重新登录；若平台改成更短的 TTL，需把预登录贴近打卡时间 |
| 校园网 802.1X / Portal 未实现 | 当前只支持 WPA2/WPA3 个人版（宿舍自建 WiFi）。**实测**：作者所在校园网 `generate_204` 返回 204（无 Portal），但**换学校/换网络务必先用 `netcheck` 判定**；改造方案见 `docs/network-uestc-wifi.md` |
| 设备只支持 **2.4 GHz** WiFi | 宿舍若只有 5 GHz 校园网，需要 2.4 GHz 中继或改用路由器/热点（`scan` 可现场确认） |
| OTA 未实现 | 目前靠 USB 重新烧录；分区表已预留双 OTA 分区 |
| 无蜂鸣器/OLED | 刻意保持最小硬件；状态通过 LED 语义 + 蓝牙查询表达 |
| 坐标为固定值 | 设备常驻宿舍，提交的是**宿舍本人在场时的实测坐标**；如需动态定位需外接 GNSS 或接入 WiFi 定位 API |

---

## 11. 开发与贡献

- **改接口**：只允许改 `firmware/src/checkin_api.cpp`（适配层隔离原则），并同步更新 `docs/api-contract.md`；
- **改业务逻辑**：先在 `tools/pc_reference/` 用 Python 验证，再移植到固件（Python 侧迭代快 10 倍）；
- **排障方法**：先"分层验证"（网络 → 登录页 → 会话 → 预热 → 入口 → 业务接口），再"二分定位"最小触发集，别靠猜（示范见 `docs/test-plan-checkin-path.md`）；
- **代码规范**：`.h`/`.cpp` 成对、Doxygen 风格注释、`Result` 统一错误码、日志强制脱敏（见 `docs/firmware-design.md` §3）；
- **分区表必须是纯 ASCII**（PlatformIO 用系统编码解析，中文注释会让构建直接失败）；
- **提交前自检**：`git status` 复核没有把 `capture/`、`*.har`、`config.json`、`out/` 带进来；本仓库 `.gitignore` 已默认忽略这些路径。

---

## 12. 致谢

本项目在复刻过程中沉淀了两条"隐形的平台规则"（**指纹决定免二次验证**、**门户预热决定微应用授权**）与一整套"先 PC 后固件、逐段判定"的验证方法，它们比代码本身更有价值——希望后来者少走一点弯路。

如果这个项目帮到了你，欢迎点个 ⭐；如果你是其他学校的同学，欢迎按 `docs/capture-guide.md` 复刻一份属于自己的实现。

**License: MIT**
