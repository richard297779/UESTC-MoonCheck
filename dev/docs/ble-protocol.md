# v0.6.0 当前 BLE 实现说明

本节优先于后面的历史设计。设备使用 Nordic UART UUID，协议版本 1，UTF-8 JSON 以一个换行分隔；不接受历史设想中的纯文本验证码或 `trust.import`。

- RX 支持 Write / Write Without Response，按字节累计至换行后入队。
- v0.6.0 修复 512 字节任务项截断完整 JSON 的问题；`hello.maxCommandBytes=4095`，加换行最多 4096 字节。旧版本无字段时按 511 字节限制。
- TX 的短回执为完整 JSON 行；长回执使用 `{seq,of,b64}`，seq 从 0 开始。先 base64 解码并合并原始字节，最后一次 UTF-8 解码，不能逐片解码中文。
- 当前通知路径要求实际 MTU 至少 60。客户端应协商 MTU、确认 CCCD 后再握手；Write 完成不是业务完成。
- 终态为同 ID 的 `evt:ok/error`；progress/result/alert 事件可能不带 ID。超时或断线不得自动重放有副作用的命令。
- Android 的实现、边界与测试见 [实施指南](../../android/docs/implementation-guide.md)；PC 控制台也使用 MTU 字节切片与带响应写入。
- 当前下行分片无消息组 ID，整组互斥仍待后续改造；BLE 配对加密与应用鉴权未实现。

---

# 蓝牙协议与设备交互设计（BLE Protocol）

> 对应 F1 / F2 / F6 与 D8（最小硬件）。目标：**手机不装任何自研 App 就能完成全部配置与运维**。
> 实现：ESP-IDF NimBLE，GATT Server。
>
> 本文上半部含早期协议设计，当前设备指令以 §4.4 与固件源码为准。当前固件还需要导入微信授权凭据并获取平台打卡票据；该流程不属于 CAS 的浏览器指纹 / 短信验证。

---

## 1. 为什么用"通用 BLE 串口服务"

直接采用 **Nordic UART Service（NUS）** 的 UUID 约定：

| 角色 | UUID | 属性 |
| --- | --- | --- |
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | — |
| RX（手机 → 设备） | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write Without Response |
| TX（设备 → 手机） | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify |

好处：**nRF Connect、LightBlue、各类"蓝牙调试助手"App、微信小程序 BLE API 都原生支持这组 UUID**，无需开发 App，也无需维护客户端。设备名（advertising name）建议 `WANAN-XXXX`（XXXX 为 MAC 后两字节），便于多设备区分。

---

## 2. 帧格式

- 编码：**UTF-8 JSON**，以 `\n` 结尾（换行既是分隔符也便于手工调试）；
- 单帧目标 < 200 B；超过协商 MTU 时按 §3 分片；
- 每条指令可带 `id`（客户端自增），设备在回执中回填 `id` 便于请求-响应配对；
- **兼容朴素写法**：也接受 `code=123456` 这类纯文本（见 §4.2）。

```jsonc
// 请求
{"op":"wifi.set","id":3,"ssid":"UESTC-WIFI","pass":"******"}
// 回执
{"id":3,"evt":"ok","msg":"wifi saved & connected","ip":"10.21.x.x"}
```

---

## 3. MTU 与分片

| 项 | 说明 |
| --- | --- |
| MTU 协商 | 连接后主动请求 MTU 512（NimBLE `ble_att_set_preferred_mtu`）；Android 端由 App 发起，iOS 视情况 |
| 有效载荷 | `MTU - 3`；保守按 180 B 设计 |
| 分片规则 | 长数据（信任 Cookie 罐、日志、证书）使用 `seq/of/data`（`data` 为 Base64） |
| 重组 | 设备侧按 `id` 聚合，超时 10 s 丢弃并回 `evt.error{code:"FRAG_TIMEOUT"}` |
| 校验 | 可选在末片带 `md5`，设备校验后回 `ok` |

```jsonc
{"op":"trust.import","id":9,"seq":0,"of":2,"data":"eyJjIjoi..."}
{"op":"trust.import","id":9,"seq":1,"of":2,"data":"..."}
{"id":9,"evt":"ok","msg":"trust imported","cookies":4,"expire_days":30}
```

---

## 4. 指令集（手机 → 设备）

### 4.1 配置类

| op | 参数 | 说明 | 回执 |
| --- | --- | --- | --- |
| `hello` | — | 握手，取固件版本与能力位 | `{ver, caps:[], mac}` |
| `status` | `verbose?` | 配置摘要（脱敏）+ 运行状态 | 见 §5 `evt.status` |
| `wifi.set` | `ssid, pass[, auth, eap{...}]` | 写 WiFi；`auth=open|wpa2|ent`；企业网上传 EAP 字段 | 连接结果 + IP |
| `wifi.scan` | — | 扫描周边 AP（含 2.4 G 可用性判定） | AP 列表 |
| `auth.set` | `user, pass[, mfa_type]` | 写统一身份认证凭据（**不回显**） | `ok` |
| `loc.set` | `lng, lat[, addr, coord_sys]` | 写打卡坐标（D4 要求的可配置项） | `ok` |
| `sched.set` | `at[, jitter_s, window_start, window_end]` | 修改打卡时间（默认 22:00 / 21:00–23:30） | `ok` |
| `net.set` | `ntp[], tls_verify, ua` | 网络参数（含 UA，R3 需要固定） | `ok` |
| `notify.set` | `url, style, on, gap_s, quota` | 手机通知（已实装，见 §4.4 与 §7） | `ok` |
| `save` | — | 落盘（部分实现为即时落盘，可省略） | `ok` |
| `factory.reset` | `confirm:"YES"` | 擦除配置与凭据（保留固件） | `ok` 后重启 |

### 4.2 二次验证中继（核心，F2）

| op | 参数 | 说明 |
| --- | --- | --- |
| `code` | `value` **或** 纯文本 `code=123456` | 回传验证码；设备立即用它继续未完成的登录流程 |
| `code.resend` | — | 请求重新发送验证码 |
| `probing` | — | 查询当前是否正处于"等待验证码"状态 |

```jsonc
{"op":"code","value":"123456"}      // JSON 形式
code=123456                          // 纯文本形式（同样接受）
```

设备在处理完成后回执：

```jsonc
{"id":5,"evt":"ok","msg":"code accepted","stage":"checkin_done"}
{"id":5,"evt":"error","code":"CODE_INVALID","msg":"code rejected","try":1,"remain":2}
```

### 4.3 运维类

| op | 参数 | 说明 |
| --- | --- | --- |
| `run` | `mode` | 立即执行：`full` / `login_only` / `checkin_only` / `probe` / `netcheck` |
| `session.import` | 分片 | 外部注入平台会话（预留能力，纯设备模式下不常用） |
| `trust.export` | 分片 | 导出当前信任态（换机/备份用） |
| `logs` | `n[, level]` | 拉取最近 n 条环形日志 |
| `time.set` | `epoch` | 手动校时 |
| `ota` | `url, token` | 触发 OTA（见 §8） |
| `reboot` | — | 重启设备 |

### 4.4 实装指令对照（当前固件）

§4.1–4.3 是**协议设计**（含尚未实现的 `trust.import` / `ota` / `notify.set` 等）。
**已实装的指令以固件为准**，未实装项会返回 `{"evt":"error","code":"INVALID_PARAM","msg":"unknown op: …"}`。

| op | 参数 | 说明 | 耗时 |
| --- | --- | --- | --- |
| `hello` | — | 固件版本 / Flash / PSRAM / 能力位 | 否 |
| `status` | — | 配置摘要（脱敏，含**当前生效账号** `"account":"<标签>"`，账号表为空时是 `"device"`）+ 网络 + 会话 + 下次计划 + 上次结果 | 否 |
| `wifi.set` | `ssid,pass` | 写 WiFi 并立即连接+校时 | 是 |
| `wifi.scan` | — | 扫描周边 AP | 是 |
| `auth.set` | `user,pass[,bfp]` | 写学号 / 密码 / 指纹（不回显） | 否 |
| `wx.import` | `slot,openid,access,refresh,expiresAt` | 导入本人微信授权凭据；槽位与账号顺序对应，0–5 | 否 |
| `wx.status` | — | 查看微信凭据槽位状态与过期时间，不回显令牌 | 否 |
| `wx.test` | `slot?` | 设备端获取打卡票据链路测试；不提交打卡 | 是 |
| `loc.set` | `lat,lng[,addr]` | 写打卡坐标 | 否 |
| `sched.set` | `at,window_end[,jitter_s,window_start]` | 写打卡时间与窗口（默认 22:00 / 21:00–23:30） | 否 |
| `auto.set` | `enabled` | 开关每日自动打卡 | 否 |
| `notify.set` | `url,style,on,gap,quota` | 配置手机通知通道（`style` 0=GET 模板 1=POST 文本 2=POST JSON）；`url` 含 token，回执只输出脱敏形式。详见 `docs/notify-design.md` | 否 |
| `notify.status` | — | 通知开关 / 格式 / 脱敏 URL / 今日已发 / 队列长度 / 重试轮次 | 否 |
| `test.notify` | `body?` | **立即真发一条**（不入队、不受去重与配额限制），用来验证通道 | 是 |
| `account.add` | `label,user,pass,bfp[,lat,lng,addr][,enabled]` | 账号表新增/覆盖成员账号（≤6）；标签已存在且不带 `enabled` → 沿用原开关（改密码不会把关掉的账号偷偷打开） | 否 |
| `account.list` | — | 列出成员账号（不含口令与指纹；每条带 `"enabled": true\|false`） | 否 |
| `account.enable` | `label,on` | 单独开关某账号：`on=false` 不登录、不提交、不进汇总分母（控制台写法 `account on <标签>` / `account off <标签>`） | 否 |
| `account.remove` / `account.clear` | `label` / — | 删除单个 / 清空全部（清空后自动退回设备自身 `auth` + `loc` 配置） | 否 |
| `multi.set` | `gap_s` | 账号之间间隔秒（默认 20，≤600）。"单人/多人"开关已取消，打几个人由账号表决定；回执 `{"gapSeconds":N,"accounts":M,"enabled":K}` | 否 |
| `test.multi` | `probe?,force?` | 按账号表顺序跑一轮（`probe` 只做登录体检）；通知**只发一条汇总** | 是 |
| `name.set` | `name` | 自定义 BLE 广播名标签 `WANAN-<标签>`（`A-Z a-z 0-9 - _`，≤12；空串=恢复蓝牙 MAC 尾号）。**即时刷新 GATT 名与广播**（无需重启），并持久化到 NVS | 否 |
| `time.sync` | — | 立即重新校时 | 是 |
| `logs` | `n` | 拉取最近 n 行环形日志 | 否 |
| `session.clear` | — | 清空 Cookie（模拟会话失效） | 否 |
| `session.set` | `value` | 注入外部 `SESSION`（调试/应急，见 `docs/api-contract.md` §8.4） | 否 |
| `factory.reset` | `confirm:"YES"` | 擦除配置与凭据后重启 | 否 |
| `reboot` | — | 重启 | 否 |
| `test.http` | `url` | 访问任意 URL：状态码 / 字节数 / 耗时 / 是否命中关键字 | 是 |
| `test.login` | `force?` | 登录链路测试；**默认只在窗口内登录**（会话 TTL≈1 h 且打卡只接受窗口内，窗口外登录会白烧一次登录次数），`force=true` 才强制。授权本身与窗口无关，靠门户预热 `GET /api/isLoginFailure`（`docs/api-contract.md` §8.7） | 是 |
| `test.chain` | `wait_s?,force?` | **全链路自检**：网络/校时 → 登录页 → 登录 → **进入微应用** → 业务接口；`wait_s` 秒内每 30 s 重探放行（≤900） | 是 |
| `test.probe` | — | 前置体检（会话 / 今日状态 / 平台时段 / 打卡点），只读 | 是 |
| `test.checkin` | `force?` | 打卡测试；`force` 仅跳过"今日已打卡"检查，**仍需在窗口内** | 是 |
| `test.time` | — | 设备时间 / 窗口配置 / 是否在窗口内 / 下次计划 | 否 |

> ⚠️ **F2 二次验证中继（`code` / `code=123456`）尚未实装**：当前阶段依赖"浏览器指纹 + 打卡窗口内登录"完全避开二次验证（见 `docs/api-contract.md` §6）。
> 分片（`seq/of`）在**设备 → 手机**方向已实装（base64 分片，用于 `status` / `logs` 等长回执）；手机 → 设备的分片尚未实装。

---

## 5. 事件集（设备 → 手机）

| evt | 载荷 | 触发时机 | 用户该做什么 |
| --- | --- | --- | --- |
| `evt.boot` | `ver, mac, reason` | 上电/重启后（若已连接） | — |
| `evt.wifi` | `state:"connected|disconnected", ip, rssi` | WiFi 状态变化 | 断线时检查网络 |
| `evt.need_code` | `type:"sms|wecom", phone:"130****2077", ttl:600, try:1` | **需要二次验证码** | **在 10 分钟内回传 `code=xxxxxx`** |
| `evt.progress` | `stage, detail` | 登录/打卡进度（`net→login→mfa→checkin→verify`） | — |
| `evt.result` | `ok, date, ts, msg` | 打卡最终结果 | 失败时人工补卡 |
| `evt.notify` | `kind, event, title, body` | 通知**入队时**镜像一份到 BLE（真正送达走 HTTPS，见 `docs/notify-design.md`） | 控制台在线就能看到；不在线也不影响推送 |
| `evt.status` | 见下 | 响应 `status` 指令 | — |
| `evt.alert` | `kind, msg` | 需人工介入（信任将过期 / 连续失败 / 风控 / 接口变更） | 按 `kind` 处理 |
| `evt.log` | `ts, level, msg` | 订阅日志（`logs --follow`） | — |
| `evt.error` | `code, msg, where` | 指令执行失败 | 按错误码处理 |

`evt.status` 示例（这是手机端唯一的"仪表盘"）：

```jsonc
{
  "evt":"status",
  "fw":"0.3.1",
  "cfg": {"device":"WANAN-1A2B","account":"A01","label":"A01","wifiSsid":"UESTC-WIFI","wifiConfigured":true,
          "user":"2023******","passwordSet":true,"fingerprintSet":true,
          "location":{"lat":30.400000,"lng":104.000000,"address":"…"},
          "schedule":{"checkinAt":"22:00","windowEnd":"23:30","jitterSeconds":30,"autoDaily":true},
          "accountGap":20,"notifyOn":true,"notifyStyle":"post_json","notifyUrl":"https://***（len=42）"},
  "net": {"state":"connected","ip":"10.21.x.x","portal":"ok","rssi":-62},
  "auth":{"trust":"valid","expire_at":"2026-11-10","mfa_pending":false},
  "last": {"date":"2026-10-11","result":"ok","ts":"22:00:07","msg":"已打卡"},
  "next": "2026-10-12T22:00:03+08:00"
}
```

---

## 6. LED 语义与按键（D8：零新增硬件）

> 多数 ESP32-S3 开发板带 1 颗 WS2812（常见 GPIO48）；若你的板子没有，用板载普通 LED 或省略——**LED 只是便利，BLE `status` 才是权威状态**。

### 6.1 LED 语义

| 状态 | 表现 |
| --- | --- |
| 未配置（无 WiFi 凭据） | 蓝色慢闪 1 Hz |
| 联网中 | 蓝色快闪 4 Hz |
| 待机（已联网、已信任） | 绿色呼吸（2 s 周期） |
| **等待验证码** | **橙色快闪 5 Hz**（配合 BLE 事件） |
| 打卡成功 | 绿色常亮 5 s |
| 打卡失败 / 需介入 | 红色双闪循环（直到下次成功） |
| 严重错误（配置损坏） | 红色慢闪 |

### 6.2 按键（复用板载 BOOT 键，GPIO0）

| 动作 | 功能 |
| --- | --- |
| 单击 | 触发一次 `probe`（体检）并通过 BLE 播报状态（若已连接） |
| 双击 | 重新进入"可配对/配置"状态（清 BLE bonding，便于换手机） |
| 长按 5 s | 触发一次**立即打卡**（`run full`，需在时间窗内） |
| 长按 10 s | 恢复出厂设置（需在 5 s 内再单击一次确认） |

> ⚠️ GPIO0 在**上电瞬间**被按住会进入下载模式；上述按键逻辑只在系统启动完成后生效（`app_main` 里注册），正常使用不受影响。

---

## 7. 通知通道：NotifyService（已实装）

**动机**：设备在宿舍、你人在外面，BLE 够不着；LED 也看不到。而设备本来就在校园网上——发一个 HTTPS 请求推送到手机，比加任何硬件都便宜。

> 完整设计（选型、限流、队列、证书分流）见 [`notify-design.md`](notify-design.md)；本节只列协议面。

| 指令 | 参数 | 说明 |
| --- | --- | --- |
| `notify.set` | `url`,`style`,`on`,`gap_s`,`quota` | 配置**默认通道**；全部可选，只改给出的项；`style` = `0` GET 模板 / `1` POST 纯文本 / `2` POST JSON（默认 2）；`gap_s` 重试通知最小间隔秒（默认 60）；`quota` 每日条数上限（默认 40，`0`＝不限，0.4.0 起**按通道独立**计数） |
| `notify.add` | `label`,`url`,`style?`,`on?`,`accounts?` | **0.4.0 起**：新增/更新一条附加通道（最多 5 条）。`accounts` = 数组或逗号串（如 `["A01","A02"]` / `"A01,A02"`），缺省＝`all` |
| `notify.remove` | `label` | 删除附加通道 |
| `notify.enable` | `label`,`on` | 单通道开关（保留 url） |
| `notify.link` | `label`,`accounts` | 修改通道的账号关联 |
| `notify.status` | — | 回 `{"on":..,"style":..,"url":"<脱敏>","gap":..,"quota":..,"sentToday":<合计>,"queued":..,"retryCount":..,"channels":[{"label","on","style","url","accounts","sentToday"}..]}`（`channels` 数组 0.4.0 起） |
| `test.notify` | `body`,`chan?` | 立刻真发一条 `Info`（绕过队列、去重与配额），回执带 HTTP 状态与响应摘要；`chan`=标签定向附加通道，缺省默认通道 |

**路由规则**（固件 `NotifyTarget`，与桌面版 `push.fan_out` 一致）：默认通道收全局事件（摘要/断网/窗口关闭）+ 单账号动作结果 + 多人轮**汇总**；附加通道只收 `accounts` 关联账号的**逐账号结果**（`all`＝所有账号但不收汇总）；去重键按通道加 `c<idx>|` 前缀，同一账号同日只推一条「打卡成功」，失败每 3 轮 1 条。

**实际推送时机**（BLE 事件 `evt.notify` 是同一批内容的本地镜像）：

| 事件 | 时机 | 频次上限 |
| --- | --- | --- |
| `StatusDigest` | 每天窗口起点 21:00 的账号表体检之后（0 个人生效也发，摘要里写清楚） | 1 条/天 |
| `CheckinOk` | 定时全部生效账号成功（**一条汇总**走默认通道；0.4.0 起逐账号结果另发关联通道）；手动补打成功/今日已打过 | 1 条/天/通道/账号 |
| `CheckinFail` | 本轮未全成功（正文含逐人结果与「将在 3 分钟后重试」） | 每 3 轮 1 条 |
| `DeadlineMissed` | 23:30 窗口关闭仍未成功（全局 1 条；0.4.0 起未打卡账号各 1 条进其关联通道） | 1 条/天/账号 |
| `NeedMfa` | 登录要求二次验证（窗口内无法处理） | 1 条/天 |
| `NetDown` | Wi-Fi 连接失败 | 1 条/小时 |

**隐私约束**：载荷只含设备广播名、时间、状态码与结果文本；**不含**学号、口令、Cookie、指纹、坐标。通知 URL 在 `status` / `notify.status` 里只回显 scheme+host+长度。

**硬约束**：通知的失败**永不**改变打卡结论（`last` 结果、LED、幂等判据都不看通知返回值）。

```jsonc
{"op":"notify.set","id":9,"url":"https://api.day.app/<你的KEY>/{title}/{body}","style":0,"on":1}
{"op":"notify.add","id":12,"label":"A02url","url":"https://sctapi.ftqq.com/<他的SendKey>.send","style":2,"accounts":["A02"]}
{"op":"notify.link","id":13,"label":"A02url","accounts":"A02,A03"}
{"op":"notify.enable","id":14,"label":"A02url","on":0}
{"op":"notify.remove","id":15,"label":"A02url"}
{"op":"notify.status","id":10}
{"op":"test.notify","id":11,"body":"manual test","chan":"A02url"}
```
---

## 8. OTA

| 项 | 设计 |
| --- | --- |
| 触发 | BLE `ota{url,token}`；默认不开启监听 |
| 传输 | `esp_https_ota` 从给定 URL 拉取固件（可先用电脑起一个临时 HTTP 服务，或用 Gitee/GitHub Release 直链） |
| 校验 | 必校验固件镜像签名（`esp_app_desc` + SHA256）；建议开启 Secure Boot v2（可选） |
| 回滚 | 双 OTA 分区；启动失败自动回滚（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`） |
| 中断安全 | 掉电/断网不破坏当前运行固件 |
| 结果 | 通过 BLE `evt.progress`/`evt.result` 与 Webhook 双通道上报 |

---

## 9. 安全

| 项 | 措施 |
| --- | --- |
| 配对 | LE Secure Connections + 静态 Passkey（6 位，印在外壳标签） |
| 权限 | 配置类特征值要求**加密链路**才可写（`BLE_GATT_CHR_F_WRITE_ENC`） |
| 暴露面 | 可选"配置窗口"：上电或双击按键后 5 分钟内可写配置，其余时间只读状态 |
| 凭据 | `auth.set` 只写不读；`status` 中账号永远打码 |
| 验证码 | 用完即弃，不落盘、不进日志 |

---

## 10. 手机端操作流程（首次配置，约 5 分钟）

1. 应用商店安装 **nRF Connect**（或任意"蓝牙调试助手"）；
2. 上电设备，扫描并连接 `WANAN-XXXX`；出现配对请求时输入外壳上的 6 位 Passkey；
3. 打开 NUS 服务，订阅 TX（Notify）；
4. 依次写入（每条都以 `\n` 结尾）：
   - `{"op":"status"}` → 确认通信正常；
   - `{"op":"wifi.scan"}` → 确认能看到 `UESTC-WIFI`；
   - `{"op":"wifi.set","ssid":"UESTC-WIFI","pass":"..."}`（企业网再加 `eap` 字段）→ 等待 `evt.wifi connected`；
   - `{"op":"auth.set","user":"2023xxxxxx","pass":"..."}`；
   - `{"op":"loc.set","lng":104.xxxxxx,"lat":30.xxxxxx,"addr":"xx宿舍"}`；
   - `{"op":"run","mode":"full"}` → 触发首次登录；
5. 若收到 `evt.need_code`，把手机收到的验证码写回：**`code=123456`**；
6. 收到 `evt.result{ok:true}` → 配置完成，设备进入无人值守状态；
7. 之后任何时候写 `{"op":"status"}` 即可查看"信任到期时间 / 上次打卡结果 / 下次打卡时间"。

---

## 11. 错误码（与 `docs/python-test-design.md` §8 保持一致）

| code | 含义 | 建议动作 |
| --- | --- | --- |
| `NET_DOWN` | 网络不可达 | 检查校园网 |
| `PORTAL_FAIL` | 校园网 Portal 认证失败 | 检查 Portal 配置 |
| `EAP_FAIL` | 802.1X 认证失败 | 检查 EAP 字段 |
| `TLS_FAIL` | 证书/握手失败 | 检查 CA |
| `BAD_CRED` | 账号密码错误 | 重新 `auth.set` |
| `NEED_CODE` | 需要验证码 | 回传 `code` |
| `CODE_INVALID` | 验证码错误 | 重新获取并回传 |
| `API_CHANGED` | 页面/接口变更 | 需 OTA 修复 |
| `RISK` | 触发风控/滑块 | 立即人工介入 |
| `WINDOW` | 不在打卡时间窗 | 等待或调整 `sched` |
| `GEO` | 坐标不在范围内 | 检查 `loc` |
| `FRAG_TIMEOUT` | 分片超时 | 重新发送 |
| `CONFIG` | 配置缺失/损坏 | 重新配置 |

---

## 12. 固件能力位（`hello` 返回，便于将来兼容）

```jsonc
{"evt":"hello","fw":"0.3.1","proto":1,
 "caps":["wifi.ent","portal.srun","mfa.relay","loc.fixed","webhook","ota","logs"]}
```
