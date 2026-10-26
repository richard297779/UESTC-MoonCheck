# 指令手册

设备只接受 **JSON 指令**（格式 `{"op":"...","id":N,...}`）。日常使用不需要记 JSON：
仓库里的 `tools/ble_console.py` 会把下面的短命令自动转成 JSON；[Android APK](../android/README.md) 提供对应按键与表单。

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
| `wx import <slot> <openid> <access> <refresh> <expiresAt>` | 导入本人微信授权凭据（`ble_console.py` 快捷命令） |
| `loc <纬度> <经度> [地址…]` | 配置打卡坐标，地址只用于显示 |
| `sched <打卡时间> [窗口结束] [抖动秒]` | 例：`sched 22:00 23:30 30` |
| `auto [on\|off]` | 开关每日自动打卡（不带参数等于 `on`） |
| `name [标签]` | 设置广播名后缀，`name A01` → `WANAN-A01`；广播即时刷新，重新扫描可见 |

示例：

```text
wifi Dorm-2.4G mywifi123
auth 2023XXXXXXX mypassword 0123456789ABCDEF0123456789ABCDEF
wx import 0 <openid Cookie> <accesstoken Cookie> <refreshtoken Cookie> 0
loc 30.753456 103.934123 四川省成都市郫都区
sched 22:00 23:30 30
auto on
```

> WiFi 名或密码含空格时，短命令会被拆错，改用原始指令：
> `raw {"op":"wifi.set","ssid":"我的 WiFi","pass":"a b#c"}`

### 微信操作票据（当前固件源码）

平台现要求打卡请求携带 `X-Checkin-Ticket`。当前 v0.6.0 固件会在打卡前自动使用导入的微信凭据换取短效票据，无需手动取得或设置票据本身。v0.5.1 已支持票据，但长命令队列存在截断问题；v0.6.0 修复后，`hello.maxCommandBytes` 为 4095（另加一个换行，总计最多 4096 字节）。没有该字段的旧固件按 511 字节 JSON 上限处理。

在 `ble_console.py` 中可直接使用以下快捷命令：

```text
wx import 0 <openid> <access> <refresh> <expiresAt>
wx status
wx test 0
```

`wx import` 的令牌会发送给设备，但不会显示在控制台回显中。使用 nRF Connect 等原始 BLE 客户端时，请将下方 JSON 指令发送到设备；在 `ble_console.py` 中则使用对应的 `wx` 快捷命令。

| 操作 | `ble_console.py` 快捷命令 | 原始 BLE JSON 指令（如 nRF Connect） | 说明 |
| --- | --- | --- |
| 导入 | `wx import <slot> <openid> <access> <refresh> <expiresAt>` | `{"op":"wx.import","slot":0,"openid":"…","access":"…","refresh":"…","expiresAt":<Unix秒>}` | 保存本人授权的微信凭据到槽位 0–5；刷新后会自动更新凭据 |
| 查看摘要 | `wx status` | `{"op":"wx.status"}` | 仅显示各槽位是否有效及过期时间，不返回凭据明文 |
| 测试 | `wx test [slot]` | `{"op":"wx.test","slot":0}` | 执行换票链路但不提交打卡；快捷命令默认槽位 0 |

`expiresAt` 为绝对 Unix 时间（秒）。扫码地址和 Cookie 获取位置见[快速开始](quickstart.md#22-准备微信授权凭据)。Cookie 的 `openid`、`accesstoken`、`refreshtoken` 分别填入 `openid`、`access`、`refresh`；`expires_in` 是剩余秒数，须加当前 Unix 时间换算成 `expiresAt`（也可传 `0` 让设备首次使用前刷新）。账号表为空时主账号使用槽位 0；账号表非空时，账号按表内顺序对应槽位 0–5。删除账号会使后续账号位置前移，因此应重新导入受影响槽位的凭据。每个账号必须由本人扫码授权，妥善保管令牌。

**导入开发板前关闭完成扫码的 QR Connect 页面。** 同一组微信授权凭据应只由一个客户端保活，避免扫码页面或其他程序与设备同时刷新令牌。

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
| `wx test [slot]` | 测试指定账号槽位的微信授权换票链路，不提交打卡（默认槽位 0） |
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
`chain` 检查 CAS / 微应用链路，不检查微信票据；使用 `ble_console.py` 时还应单独运行 `wx test 0`。

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
| `notify set url=<地址> [style=0\|1\|2] [on=1] [gap=秒] [quota=条]` | 配置默认通道（收全部事件） |
| `notify add <标签> url=<地址> [style=..] [on=1] [accounts=A01,A02\|all]` | 新增附加通道（0.4.0 起，最多 5 条） |
| `notify rm <标签>` / `notify on <标签>` / `notify off <标签>` | 删除 / 启用 / 停用某条附加通道 |
| `notify link <标签> <A01,A02\|all>` | 改通道关联的账号（只收这些账号的结果） |
| `notify test [chan=<标签>] [文本]` | 立即发送一条测试消息（不入队；默认走默认通道） |
| `notify` | 列出所有通道与今日已发数 |

`style`：`0` = GET 模板（Bark）、`1` = 纯文本（ntfy）、`2` = JSON（Server酱 / 中转）。

多人模式下：逐账号结果推给**关联该账号**的通道，整轮汇总只走默认通道。
详见 **[手机通知](notify.md)**。

---

## 完整 JSON 指令表

需要自己写脚本或用 nRF Connect 时查这张表。

| op | 参数 | 说明 |
| --- | --- | --- |
| `hello` | — | 版本与硬件信息 |
| `status` | — | 全量状态 |
| `wx.import` | `slot`, `openid`, `access`, `refresh`, `expiresAt` | 导入微信授权凭据（slot 0–5） |
| `wx.status` | — | 查看微信凭据槽位摘要（不含令牌） |
| `wx.test` | `slot?` | 测试设备端票据获取，不提交打卡 |
| `wifi.set` | `ssid`, `pass` | 连接 WiFi |
| `wifi.scan` | — | 扫描 2.4 GHz |
| `auth.set` | `user`, `pass`, `bfp` | 主账号 |
| `loc.set` | `lat`, `lng`, `addr` | 坐标 |
| `sched.set` | `at`, `window_end`, `jitter_s` | 调度 |
| `auto.set` | `enabled` | 自动打卡开关 |
| `notify.set` | `url`, `style`, `on`, `gap`, `quota` | 默认通道 |
| `notify.add` | `label`, `url`, `style?`, `on?`, `accounts?` | 附加通道（≤5 条，0.4.0 起；`accounts` 数组或逗号串，缺省=all） |
| `notify.remove` | `label` | 删除附加通道 |
| `notify.enable` | `label`, `on` | 附加通道开关 |
| `notify.link` | `label`, `accounts` | 改关联账号 |
| `notify.status` | — | 通知状态（含 `channels` 列表与每通道今日已发） |
| `test.notify` | `body`, `chan?` | 立即发一条（`chan` 定向附加通道） |
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
{"id":3,"op":"wx.import","slot":0,"openid":"<本人openid>","access":"<access token>","refresh":"<refresh token>","expiresAt":1792890000}
{"id":4,"op":"loc.set","lat":30.753456,"lng":103.934123,"addr":"xx 宿舍"}
{"id":5,"op":"sched.set","at":"22:00","window_end":"23:30","jitter_s":30}
{"id":6,"op":"auto.set","enabled":true}
{"id":7,"op":"wx.test","slot":0}
{"id":8,"op":"test.chain","wait_s":90}
```

每发一条，TX 里会收到对应 `id` 的回执。`wx.test` 和 `test.chain` 都通过后，才分别确认微信票据链路与 CAS / 微应用链路正常。

> 手机 App 一次只能发一条，注意等上一条回执出来再发下一条。
