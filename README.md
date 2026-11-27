# 🌙 MoonCheck

**ESP32 晚安成电打卡终端** · [v0.9.0 最新发行](https://github.com/richard297779/UESTC-MoonCheck/releases/tag/v0.9.0) · [独立桌面版](desktop/README.md)

一块常驻宿舍的 ESP32，蓝牙配置一次，之后每晚自动完成「登录 → 门户预热 → 进入微应用 → 获取微信操作票据 → 定位打卡 → 结果确认」。配置完成后不需要电脑，也不用每天操作。

> 本项目按**电子科技大学「晚安成电」**平台实现（打卡窗口 21:00–23:30）。其他学校的平台不同，需自行适配源码。

---

## 新版晚安成电小组确认

当前源码支持安全员通过网页批量确认当天待处理的小组记录。固件和桌面版都沿用已有调度，使用学校门户登录。新增账户默认启用小组模式。每个账户独立设置，升级时现有账户继承原设备打卡模式。

- ESP32 使用更新后的固件后，可在电脑 BLE 控制台「查询与配置 → 账户配置」或 Android「账户」编辑对应账户。命令行可执行 `group on <账号>`。操作见 [固件小组模式](firmware/README.md#新版小组确认)。
- 独立桌面版在「账户配置」中按账户设置小组模式。操作见 [桌面小组模式](desktop/README.md#新版小组确认)。

如发现组员无法联系，在计划运行前到网页提交异常，或先暂停本轮自动操作。程序根据平台待确认名单提交“情况正常”，并复查待确认人数。小组确认随 v0.9.0 发行。

## 功能

- **蓝牙配网**：手机 nRF Connect 或电脑 `ble_console.py` 即可完成全部配置；电脑支持[双击图形控制台](docs/ble-gui.md)。
- **自动获取打卡操作票据**：当前固件源码内置微信小程序授权链，打卡前自动申请平台要求的 `X-Checkin-Ticket`。
- **一次配置，长期有效**：CAS 登录使用浏览器指纹避免重复短信验证；微信授权凭据用于获取打卡操作票据。
- **每晚自动打卡**：21:00 预登录，22:00 后 0–30 秒随机延迟提交，失败每 3 分钟重试到 23:30。
- **结果可查**：打卡后自动复查，结果写入设备，掉电不丢；`status` / `probe` 随时查询。
- **掉电自恢复**：重启后自动重连 WiFi、校时、继续调度。
- **自带诊断**：`chain` 全链路自检、`netcheck` 判断校园网认证、`scan` 看 2.4 GHz、`logs` 回读日志。
- **手机通知**（可选）：Bark / ntfy / Server酱 / Qmsg / 局域网中转，成功、失败、重试都有推送；支持**多通道**（每人一条通道，自己的结果只推给自己）。
- **一台设备多人**：账号表最多 6 人，可单独开关（需每位账号持有者知情同意）。
- **多板型构建**：保留 S3-N16R8 与 WROOM-32，新增 WROVER-4MB、S3-N8/N8R8、C3-DevKitM-1 实验目标。新增型号未经实机验证，欢迎提供脱敏验证反馈，见[板型支持矩阵](docs/board-support.md)。
- **APK 运行状态**：v0.7.0 增加实际联网名称、内存峰值、PSRAM、运行时长与开发者脱敏日志。
- **桌面版 1.2.0**支持安全员小组确认，继续提供个人打卡所用的微信授权与取票。Windows 本地验证包独立构建。真实账号已验证小组登录和只读计数，真实提交待验证。详见[桌面说明](desktop/README.md)。
- **蓝牙中控**支持每账户设置打卡模式、留空保留凭据、匹配微信授权目标及指定账号执行。双击 `tools/ble_console.pyw` 打开新版图形界面。详见[使用说明](docs/ble-gui.md)。
- 电脑 BLE 控制台源码随 [v0.9.0](https://github.com/richard297779/UESTC-MoonCheck/releases/tag/v0.9.0) 提供。独立 Windows 打卡程序的已发布版本为 [desktop-v1.1.0 预发行版](https://github.com/richard297779/UESTC-MoonCheck/releases/tag/desktop-v1.1.0)。
- **内置微信扫码授权**：Android 和电脑 BLE GUI 可获取、刷新二维码，在本人微信确认后回填三 Cookie；不自动发送设备。[实现与验证](dev/docs/qr-authorization.md)记录验证范围；本轮用户已确认基本功能，未提供逐项扫码闭环或兼容矩阵明细。

---

## 你需要准备

| 项目 | 说明 |
| --- | --- |
| 开发板 | ESP32-S3-N16R8（推荐）或 ESP32-WROOM-32（4 MB） |
| 供电 | USB 5 V 常供电（≥ 500 mA） |
| 网络 | 2.4 GHz WiFi；校园网需能直接上网（`netcheck` 返回 204） |
| 电脑 | Windows / macOS / Linux，Chrome 或 Edge，Python 3.10+ |
| 信息 | 学号、统一身份认证密码、浏览器指纹、微信授权凭据、宿舍坐标、WiFi 名与密码 |

> 坐标**必须在宿舍现场取**：打卡围栏只有 130 米。

---

## 快速开始

### 1. 刷固件

v0.9.0 已在 ESP32-S3-N16R8 上由用户确认配置和真实打卡成功。ESP32-WROOM-32 构建通过，当前版本实机待验证。四种实验板型可按[板型支持说明](docs/board-support.md)自行构建。

从 [v0.9.0 发行页](https://github.com/richard297779/UESTC-MoonCheck/releases/tag/v0.9.0) 下载对应板型固件。`MoonCheck-0.9.0-release.zip` 包含两种常规板型固件、Android Debug APK、电脑 BLE 控制台源码与使用说明。网页烧录见[刷固件](docs/flashing.md)，自行构建见[固件说明](firmware/README.md)。

> 详细步骤、升级（保留配置）和报错处理见 **[刷固件](docs/flashing.md)**。

> **版本范围：** v0.6.0 新增 Android 控制端并修复长配置截断；APK 实际联调已由用户确认通过、功能全部正常，当时 v0.6.0 转为 Latest；当前 Latest 为 v0.9.0；APK 仍使用原 Debug 签名。旧 v0.5.1 已支持微信票据，但长 JSON 命令会受旧队列限制。

### 2. 准备授权信息与宿舍坐标

| 值 | 怎么取 |
| --- | --- |
| 浏览器指纹 | 用于 CAS 登录免短信验证：Chrome 登录 `jzsz.uestc.edu.cn` → F12 → Application → Cookies → `https://idas.uestc.edu.cn` → `MULTIFACTOR_BROWSER_FINGERPRINT`（32 位十六进制） |
| 微信授权凭据（个人定位模式） | 每个账号由本人通过 PC 端 QR Connect 扫码授权获取；操作步骤与字段转换见[快速开始](docs/quickstart.md#22-准备微信授权凭据)。 |
| 宿舍坐标（个人定位模式） | 人在宿舍打开晚安成电页面 → F12 → Network 过滤 `reverse` → 从 `map/reverse?lat=…&lng=…` 读取 |

浏览器指纹是 HttpOnly，`document.cookie` 看不到；必须用你**平时打卡的那台浏览器**。微信授权凭据与密码同等敏感，只能导入本人账号并妥善保管。

### 3. 蓝牙配置

安卓用户安装与固件配套的 APK，在「配置」保存 Wi-Fi 和计划，在「账户」填写本人凭据并选择模式。个人定位账户另需位置和本人微信授权。详情见 [Android 使用说明](android/README.md)。

电脑用户：

```powershell
python -m pip install -r tools/requirements-ble.txt
python tools/ble_console.py --list          # 找到 WANAN-XXXX
python tools/ble_console.py --name WANAN-XXXX
```

连接后可用图形界面完成「账户配置」和「计划与通知」。命令行示例使用小组模式，密码在终端隐藏输入。

```text
wifi <WiFi名> <WiFi密码>
account list
account new <学号> <32 位浏览器指纹>
sched 22:00 23:30 30
```

个人定位账户可在新建命令末尾加 `personal`，再设置位置并导入微信授权。验证通过后按需执行 `auto on`。

`expiresAt` 使用 Unix 绝对时间（秒）；扫码 Cookie 的 `expires_in` 是剩余秒数，需加当前 Unix 时间换算，也可填 `0` 让固件首次使用前刷新（推荐）。扫码及 Cookie 提取步骤见[快速开始](docs/quickstart.md#22-准备微信授权凭据)。复制 Cookie 后、导入前关闭 QR Connect 页面，确保同一授权只有开发板一个客户端保活。在 `ble_console.py` 中执行 `wx status` 确认槽位已保存，再用 `wx test 0` 测试微信授权链；该测试只取票、不提交打卡。多人账号按账号表顺序分别导入槽位 0–5，见[多人打卡](docs/multi-account.md)。

### 4. 验证

```text
chain 0
```

看到 `appEntry: true` 就说明平台已授权这台设备。白天 `inWindow: false` 是正常的。
`chain` 不检查微信票据；新源码还应确认 `wx test 0` 成功。

21:00–23:30 之间可以手动打一次：

```text
checkin
```

之后就不用再管了。

---

## 每天晚上发生什么

| 时间 | 动作 |
| --- | --- |
| 21:00 | 预取微信票据；提交前仍会检查有效性 |
| 21:05 | 预登录体检与门户预热 |
| 22:00 后 0–30 秒 | 提交打卡；必要时刷新会话或票据 |
| 失败后 | 每 3 分钟重试，直到 23:30 |
| 打卡后 | 复查状态，结果写入设备 |

---

## 手机通知（可选）

```text
# Bark
notify set url=https://api.day.app/<你的KEY>/{title}/{body} style=0

# ntfy
notify set url=https://ntfy.sh/<你的主题名> style=1

# Server酱
notify set url=https://sctapi.ftqq.com/<你的SendKey>.send style=2

notify test 你好
```

0.4.0 起支持多通道：**每个人的结果单独推给每个人**（最多再挂 5 条附加通道）：

```text
notify add A02url url=https://sctapi.ftqq.com/<他的SendKey>.send style=2 accounts=A02
```

Bark、ntfy、Server酱及 Qmsg 的配置方法、事件说明、路由规则与排错见 **[手机通知](docs/notify.md)**。

---

## 一台设备多人打卡

前提：**每一位账号持有者都知情并同意**。

```text
account list
account new <学号> <32位浏览器指纹>    # 最多 6 个
account on A01                          # 将 A01 换成列表中的标签
multi probe                             # 只读演练，不提交
notify add A01url url=… accounts=A01    # 结果单独推给他
```

详见 **[多人打卡](docs/multi-account.md)**。不想用硬件？同款核心的 [桌面版](desktop/README.md)（Windows/Linux 单文件）也支持多账号与逐人推送。

---

## 文档

| 文档 | 内容 |
| --- | --- |
| [快速开始](docs/quickstart.md) | 从刷好固件到自动打卡，含截图 |
| [刷固件](docs/flashing.md) | 网页刷写、升级、换分区表、报错处理 |
| [指令手册](docs/commands.md) | 全部蓝牙命令与 JSON 指令 |
| [手机通知](docs/notify.md) | Bark、ntfy、Server酱、Qmsg 和局域网中转，多通道分发 |
| [多人打卡](docs/multi-account.md) | 账号表与逐个开关 |
| [桌面版](desktop/README.md) | 不烧板子：Windows/Linux 单文件版（CLI+图形界面） |
| [Android 控制端](android/README.md) | BLE 按键控制、配置向导、账号与离线帮助（开发测试版） |
| [项目体检](docs/project-health.md) | 当前版本验证及历次发行记录 |
| [板型支持](docs/board-support.md) | 构建环境、硬件规格、验证状态与反馈 |
| [常见问题](docs/faq.md) | 排错速查与已知限制 |
| [发行说明](release/README.md) | 版本变化与升级注意 |
| [自行编译](firmware/README.md) | 用 PlatformIO 从源码构建 |
| [开发者文档](dev/README.md) | 原理、协议、设计文档与开发工具（想读懂/复刻看这里） |

---

## 常见问题

- BLE 搜不到 / 连接超时
- `4001 登录无效或已过期`
- `NEED_MFA`（要验证码）
- 打卡距离过大 / 记录异常
- 通知只有标题、没有正文

完整排查见 **[常见问题](docs/faq.md)**。

---

## 已知限制

- 会话 TTL 约 1 小时，所以每天 21:00 重新登录；平台规则变化时需同步调整。
- 只支持 2.4 GHz WiFi；802.1X / 网页认证（Portal）校园网不支持。
- 坐标固定为配置值，设备需常驻打卡范围内。
- 二次验证码中继未实现：指纹失效时会报 `NEED_MFA`，需重新取指纹。
- 固件升级需 USB 刷写（OTA 未实现）。
- 微信授权凭据会在设备 NVS 中保存；若凭据过期且刷新失败，需要重新导入本人账号的授权凭据。
- 通知队列在内存中，重启会丢失尚未发送的通知。

---

## 免责声明

- 仅用于**本人账号**；多人使用必须获得每位账号持有者的明确同意。不提供代打服务，不用于营利。
- 自动化打卡**可能不符合学校规定**，请自行评估并承担后果。
- 设备内明文保存学号与密码，请勿转借；丢失后立即修改密码。
- 本项目按 MIT 协议开源，作者不对使用后果承担责任。

## License

MIT
