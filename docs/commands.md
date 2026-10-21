# 指令手册

设备只接受 **JSON 指令**（格式 `{"op":"...","id":N,...}`）。日常使用不需要记 JSON：
仓库里的 `tools/ble_console.py` 会把下面的短命令自动转成 JSON。

```powershell
python tools/ble_console.py --name WANAN-XXXX
```

连上后输入 `help` 可以看到命令列表。所有回执都是 JSON，`"evt":"ok"` 表示成功，
`"evt":"error"` 表示失败，看 `msg` 和 `code`。

---

## 配置类

| 命令 | 说明 |
| --- | --- |
| `wifi <ssid> [密码]` | 连接 2.4 GHz WiFi；成功后自动校时 |
| `auth <学号> <密码> [指纹]` | 配置主账号；指纹可选但强烈建议填 |
| `loc <纬度> <经度> [地址…]` | 配置打卡坐标，地址只用于显示 |
| `sched <打卡时间> [窗口结束] [抖动秒]` | 例：`sched 22:00 23:30 30` |
| `auto [on\|off]` | 开关每日自动打卡（不带参数等于 `on`） |
| `name [标签]` | 设置广播名后缀，`name A01` → `WANAN-A01`；重启生效 |

示例：

```text
wifi Dorm-2.4G mywifi123
auth 2023XXXXXXX mypassword 0123456789ABCDEF0123456789ABCDEF
loc 30.753456 103.934123 四川省成都市郫都区
sched 22:00 23:30 30
auto on
```

> WiFi 名或密码含空格时，短命令会被拆错，改用原始指令：
> `raw {"op":"wifi.set","ssid":"我的 WiFi","pass":"a b#c"}`

---

## 查询类

| 命令 | 说明 |
| --- | --- |
| `hello` | 版本、MAC、Flash 大小、PSRAM |
| `status` | 全量状态：网络、配置、调度、上次结果（敏感值自动打码） |
| `time` | 设备当前时间 |
| `scan` | 扫描附近 2.4 GHz WiFi（`ssid/rssi/channel/enc`） |
| `netcheck` | 访问 `connect.rom.miui.com/generate_204`，204 表示可直接上网，302/HTML 表示需要网页认证 |
| `logs [n]` | 回读最近 n 行日志，默认 30 |
| `notify` | 查看通知配置 |
| `account list` | 查看账号表 |

---

## 测试类

| 命令 | 说明 |
| --- | --- |
| `login [force]` | 登录。窗口外默认跳过（保护会话），`force` 强制 |
| `probe` | 只读体检：不登录、不打卡，只看平台会话是否可用 |
| `chain [秒] [force]` | 全链路自检：登录页 → 会话 → H5 → 门户预热 → 进入微应用 |
| `checkin [force]` | 提交打卡。只在 21:00–23:30 有效；成功后自动复查 |
| `http <url>` | 用设备网络发一次 GET，排查 DNS / TLS / 代理 |
| `raw <json>` | 直接发送 JSON |

### `chain` 回执怎么读

```json
{
  "loginPage": true,
  "session": true,
  "h5Page": true,
  "warmup": true,
  "appEntry": true,
  "today": "PENDING",
  "window": "21:00 - 23:30",
  "inWindow": false,
  "canCheckin": false
}
```

| 字段 | 含义 |
| --- | --- |
| `loginPage` | 能打开统一身份认证登录页（网络 / TLS 正常） |
| `session` | 已拿到或已有 CAS 会话 |
| `h5Page` | 微应用 H5 页面可访问 |
| `warmup` | 门户预热成功（**这步不做，后面一定失败**） |
| `appEntry` | 平台已授权本设备进入打卡微应用 |
| `today` | 今天的打卡状态：`PENDING` / `CHECKED_IN` / … |
| `inWindow` | 现在是否在打卡窗口内 |
| `canCheckin` | 现在是否能直接打卡 |

白天看到 `inWindow: false` 是完全正常的。

---

## 会话与维护

| 命令 | 说明 |
| --- | --- |
| `session <值>` | 手动写入会话 Cookie（抓包调试用，一般不需要） |
| `clear` | 清空会话，下次重新登录 |
| `reset` | 恢复出厂设置（需输入 `YES` 确认；配置、账号表、会话全清） |
| `quit` | 断开蓝牙退出（**不会丢配置**） |

---

## 多账号

| 命令 | 说明 |
| --- | --- |
| `account add <标签> <学号> <密码> [指纹]` | 添加账号，标签如 `A01`，最多 6 个 |
| `account list` | 列出所有账号及开关状态 |
| `account on <标签>` / `account off <标签>` | 单独启用 / 暂停某账号 |
| `account rm <标签>` | 删除 |
| `account clear` | 清空账号表 |
| `multi` / `multi probe` / `multi force` | 按账号表执行一轮；`probe` 只读、不打卡 |
| `multi <间隔秒>` | 设置账号之间的间隔（0–600），不触发执行 |

账号表为空时，设备使用 `auth` 配的那个主账号。

---

## 通知

| 命令 | 说明 |
| --- | --- |
| `notify set url=<地址> [style=0\|1\|2] [on=1] [gap=秒] [quota=条]` | 配置通知 |
| `notify test [文本]` | 立即发送一条测试消息（不入队） |

`style`：`0` = GET 模板（Bark）、`1` = 纯文本（ntfy）、`2` = JSON（Server酱 / 中转）。

详见 **[手机通知](notify.md)**。

---

## 完整 JSON 指令表

需要自己写脚本或用 nRF Connect 时查这张表。

| op | 参数 | 说明 |
| --- | --- | --- |
| `hello` | — | 版本与硬件信息 |
| `status` | — | 全量状态 |
| `wifi.set` | `ssid`, `pass` | 连接 WiFi |
| `wifi.scan` | — | 扫描 2.4 GHz |
| `auth.set` | `user`, `pass`, `bfp` | 主账号 |
| `loc.set` | `lat`, `lng`, `addr` | 坐标 |
| `sched.set` | `at`, `window_end`, `jitter_s` | 调度 |
| `auto.set` | `enabled` | 自动打卡开关 |
| `notify.set` | `url`, `style`, `on`, `gap`, `quota` | 通知 |
| `notify.status` | — | 通知状态 |
| `test.notify` | `body` | 立即发一条 |
| `account.add` | `label`, `user`, `pass`, `bfp`, `lat`, `lng`, `addr` | 添加账号 |
| `account.list` | — | 账号表 |
| `account.remove` | `label` | 删除账号 |
| `account.enable` | `label`, `on` | 开关账号 |
| `account.clear` | — | 清空账号表 |
| `multi.set` | `gap_s` | 多人之间的间隔秒数（0–600） |
| `test.multi` | `probe`, `force` | 逐个执行（probe 只读） |
| `name.set` | `name` | 广播名后缀 |
| `time.sync` | — | 校时 |
| `logs` | `n` | 回读日志 |
| `session.clear` | — | 清空会话 |
| `session.set` | `value` | 写会话 Cookie |
| `factory.reset` | `confirm: "YES"` | 恢复出厂 |
| `reboot` | — | 重启 |
| `test.time` | — | 校时测试 |
| `test.http` | `url` | 网络测试 |
| `test.login` | `force` | 登录测试 |
| `test.chain` | `wait_s`, `force` | 全链路自检 |
| `test.probe` | — | 只读体检 |
| `test.checkin` | `force` | 打卡 |

带 `id` 字段便于匹配回执，例如：

```json
{"id":7,"op":"loc.set","lat":30.753456,"lng":103.934123,"addr":"xx 宿舍"}
```

---

## 用 nRF Connect 配置

没有电脑时，安卓手机 + **nRF Connect for Mobile** 也能完成全部配置。

1. 打开 App，扫描并连接 `WANAN-XXXX`。
2. 找到 Nordic UART Service：
   - 订阅 **`6E400003-B5A3-F393-E0A9-E50E24DCCA9E`**（TX，设备 → 手机，用来收回执）
   - 写 **`6E400002-B5A3-F393-E0A9-E50E24DCCA9E`**（RX，手机 → 设备）
3. 点 RX 右侧的向上箭头，类型选 **UTF-8**，粘贴一条 JSON，发送。

依次发送：

```json
{"id":1,"op":"wifi.set","ssid":"Dorm-2.4G","pass":"mywifi123"}
{"id":2,"op":"auth.set","user":"2023XXXXXXX","pass":"mypassword","bfp":"0123456789ABCDEF0123456789ABCDEF"}
{"id":3,"op":"loc.set","lat":30.753456,"lng":103.934123,"addr":"xx 宿舍"}
{"id":4,"op":"sched.set","at":"22:00","window_end":"23:30","jitter_s":30}
{"id":5,"op":"auto.set","enabled":true}
{"id":6,"op":"test.chain","wait_s":90}
```

每发一条，TX 里会收到对应 `id` 的回执。`test.chain` 返回 `appEntry:true` 就算配置成功。

> 手机 App 一次只能发一条，注意等上一条回执出来再发下一条。
