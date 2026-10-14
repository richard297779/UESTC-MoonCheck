# 蓝牙协议与设备交互设计（BLE Protocol）

> 对应 F1 / F2 / F6 与 D8（最小硬件）。目标：**手机不装任何自研 App 就能完成全部配置与运维**。
> 实现：ESP-IDF NimBLE，GATT Server。

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
| `notify.set` | `webhook_url` | 可选 Webhook（见 §7） | `ok` |
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
| `status` | — | 配置摘要（脱敏）+ 网络 + 会话 + 下次计划 + 上次结果 | 否 |
| `wifi.set` | `ssid,pass` | 写 WiFi 并立即连接+校时 | 是 |
| `wifi.scan` | — | 扫描周边 AP | 是 |
| `auth.set` | `user,pass[,bfp]` | 写学号 / 密码 / 指纹（不回显） | 否 |
| `loc.set` | `lat,lng[,addr]` | 写打卡坐标 | 否 |
| `sched.set` | `at,window_end[,jitter_s,window_start]` | 写打卡时间与窗口（默认 22:00 / 21:00–23:30） | 否 |
| `auto.set` | `enabled` | 开关每日自动打卡 | 否 |
| `name.set` | `name` | 自定义 BLE 广播名标签 `WANAN-<标签>`（`A-Z a-z 0-9 - _`，≤12；空串=恢复蓝牙 MAC 尾号）。**即时刷新 GATT 名与广播**（无需重启），并持久化到 NVS | 否 |
| `time.sync` | — | 立即重新校时 | 是 |
| `logs` | `n` | 拉取最近 n 行环形日志 | 否 |
| `session.clear` | — | 清空 Cookie（模拟会话失效） | 否 |
| `session.set` | `value` | 注入外部 `SESSION`（调试/应急，见 `docs/api-contract.md` §8.4） | 否 |
| `factory.reset` | `confirm:"YES"` | 擦除配置与凭据后重启 | 否 |
| `reboot` | — | 重启 | 否 |
| `test.http` | `url` | 访问任意 URL：状态码 / 字节数 / 耗时 / 是否命中关键字 | 是 |
| `test.login` | `force?` | 登录链路测试；**默认窗口外不登录**（平台不授权窗口外会话），`force=true` 才强制 | 是 |
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
| `evt.status` | 见下 | 响应 `status` 指令 | — |
| `evt.alert` | `kind, msg` | 需人工介入（信任将过期 / 连续失败 / 风控 / 接口变更） | 按 `kind` 处理 |
| `evt.log` | `ts, level, msg` | 订阅日志（`logs --follow`） | — |
| `evt.error` | `code, msg, where` | 指令执行失败 | 按错误码处理 |

`evt.status` 示例（这是手机端唯一的"仪表盘"）：

```jsonc
{
  "evt":"status",
  "fw":"0.3.1",
  "cfg": {"wifi":"UESTC-WIFI","user":"2023******","loc":[104.xxxxxx,30.xxxxxx],
          "sched":"22:00 / 21:00-23:30","webhook":true},
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

## 7. 可选通知通道：Webhook（纯软件，零硬件成本）

**动机**：设备在宿舍、你人在外面，BLE 够不着；LED 也看不到。而设备本来就在校园网上——发一个 HTTPS 请求通知你，比加任何硬件都便宜。

| 方案 | 成本 | 说明 |
| --- | --- | --- |
| 企业微信群机器人 | 免费 | 需要一个群 + 机器人 Webhook URL |
| PushPlus / Server 酱 | 免费额度 | 微信推送，注册即得 URL |
| 自建（如有服务器） | — | 任意 HTTP 端点 |

**推送时机（建议默认开启，可关闭）**：

| 时机 | 内容 |
| --- | --- |
| 打卡成功 | `✅ 10-11 22:00 已打卡` |
| 打卡失败（22:00 首败） | `❌ 首次打卡失败：<原因>，将在窗口内重试` |
| 需要验证码时无人应答 | `⚠️ 需要短信验证码，请连接设备回传` |
| 信任即将过期（提前 3 天） | `⏳ 信任有效期至 11-10，请安排重新验证` |
| 连续 3 次失败 / 检出风控 | `🚨 需人工介入：<原因>` |

**隐私约束**：Webhook 载荷**只包含时间戳、成功/失败、错误类别**；不含学号、密码、Cookie、坐标。

```jsonc
{"op":"notify.set","url":"https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=***"}
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
