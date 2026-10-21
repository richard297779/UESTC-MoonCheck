# 🌙 MoonCheck

**ESP32 宿舍自动定位打卡终端** · 固件 v0.3.1

一块常驻宿舍的 ESP32，蓝牙配置一次，之后每晚自动完成「登录 → 门户预热 → 进入微应用 → 定位打卡 → 结果确认」。配置完成后不需要电脑，也不用每天操作。

> 本项目按**电子科技大学「晚安成电」**平台实现（打卡窗口 21:00–23:30）。其他学校的平台不同，需自行适配源码。

---

## 功能

- **蓝牙配网**：手机 nRF Connect 或电脑 `ble_console.py` 即可完成全部配置。
- **一次配置，长期有效**：固定浏览器指纹后，登录不再需要短信验证码。
- **每晚自动打卡**：21:00 预登录，22:00 提交（±30 秒抖动），失败每 3 分钟重试到 23:30。
- **结果可查**：打卡后自动复查，结果写入设备，掉电不丢；`status` / `probe` 随时查询。
- **掉电自恢复**：重启后自动重连 WiFi、校时、继续调度。
- **自带诊断**：`chain` 全链路自检、`netcheck` 判断校园网认证、`scan` 看 2.4 GHz、`logs` 回读日志。
- **手机通知**（可选）：Bark / ntfy / Server酱 / 局域网中转，成功、失败、重试都有推送。
- **一台设备多人**：账号表最多 6 人，可单独开关（需每位账号持有者知情同意）。
- **两种板型**：ESP32-S3（16 MB，推荐）与经典 ESP32-WROOM-32（4 MB）。

---

## 你需要准备

| 项目 | 说明 |
| --- | --- |
| 开发板 | ESP32-S3-N16R8（推荐）或 ESP32-WROOM-32（4 MB） |
| 供电 | USB 5 V 常供电（≥ 500 mA） |
| 网络 | 2.4 GHz WiFi；校园网需能直接上网（`netcheck` 返回 204） |
| 电脑 | Windows / macOS / Linux，Chrome 或 Edge，Python 3.10+ |
| 信息 | 学号、统一身份认证密码、浏览器指纹、宿舍坐标、WiFi 名与密码 |

> 坐标**必须在宿舍现场取**：打卡围栏只有 130 米。

---

## 快速开始

### 1. 刷固件

从 [Releases](https://github.com/richard297779/UESTC-MoonCheck/releases) 下载对应板型的
`wanan-<版本>-<板型>-merged.bin`，用 Chrome / Edge 打开
<https://espressif.github.io/esptool-js/>，地址填 `0x0` 刷入。

> 详细步骤、升级（保留配置）和报错处理见 **[刷固件](docs/flashing.md)**。

### 2. 取两个值

| 值 | 怎么取 |
| --- | --- |
| 浏览器指纹 | Chrome 登录 `jzsz.uestc.edu.cn` → F12 → Application → Cookies → `https://idas.uestc.edu.cn` → `MULTIFACTOR_BROWSER_FINGERPRINT`（32 位十六进制） |
| 宿舍坐标 | 人在宿舍打开晚安成电页面 → F12 → Network 过滤 `reverse` → 从 `map/reverse?lat=…&lng=…` 读取 |

指纹是 HttpOnly，`document.cookie` 看不到；必须用你**平时打卡的那台浏览器**。

### 3. 蓝牙配置

```powershell
python -m pip install bleak
python tools/ble_console.py --list          # 找到 WANAN-XXXX
python tools/ble_console.py --name WANAN-XXXX
```

连上后依次输入：

```text
wifi <WiFi名> <WiFi密码>
auth <学号> <密码> <指纹>
loc <纬度> <经度> 四川省成都市郫都区
sched 22:00 23:30 30
auto on
```

### 4. 验证

```text
chain 0
```

看到 `appEntry: true` 就说明平台已授权这台设备。白天 `inWindow: false` 是正常的。

21:00–23:30 之间可以手动打一次：

```text
checkin
```

之后就不用再管了。

---

## 每天晚上发生什么

| 时间 | 动作 |
| --- | --- |
| 21:00 | 自动预登录（会话只有约 1 小时，不能提前太多） |
| 22:00 ± 30 秒 | 提交打卡 |
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

Bark / ntfy / Server酱 的申请方式、事件说明和排错见 **[手机通知](docs/notify.md)**。

---

## 一台设备多人打卡

前提：**每一位账号持有者都知情并同意**。

```text
account add A01 <学号> <密码> [指纹]    # 最多 6 个
account on A01                          # 或 account off A01
multi probe                             # 只读演练，不提交
```

详见 **[多人打卡](docs/multi-account.md)**。

---

## 文档

| 文档 | 内容 |
| --- | --- |
| [快速开始](docs/quickstart.md) | 从刷好固件到自动打卡，含截图 |
| [刷固件](docs/flashing.md) | 网页刷写、升级、换分区表、报错处理 |
| [指令手册](docs/commands.md) | 全部蓝牙命令与 JSON 指令 |
| [手机通知](docs/notify.md) | Bark / ntfy / Server酱 / 局域网中转 |
| [多人打卡](docs/multi-account.md) | 账号表与逐个开关 |
| [常见问题](docs/faq.md) | 排错速查与已知限制 |
| [发行说明](release/notes.md) | 版本变化与升级注意 |
| [自行编译](firmware/README.md) | 用 PlatformIO 从源码构建 |
| [PC 验证工具](tools/pc_reference/README.md) | 先在电脑上验证链路（可选） |

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
- 通知队列在内存中，重启会丢失尚未发送的通知。

---

## 免责声明

- 仅用于**本人账号**；多人使用必须获得每位账号持有者的明确同意。不提供代打服务，不用于营利。
- 自动化打卡**可能不符合学校规定**，请自行评估并承担后果。
- 设备内明文保存学号与密码，请勿转借；丢失后立即修改密码。
- 本项目按 MIT 协议开源，作者不对使用后果承担责任。

## License

MIT
