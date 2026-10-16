# 通知功能设计（NotifyService）

> 版本 v1.1（2026-10-18；v1.0 实装于 2026-10-17）。模块：`firmware/include/notify_service.h` + `firmware/src/notify_service.cpp`。
> 需求来自用户的 5 条决策：① 通知要能到手机；② 每天 21:00 固定一条状态消息；③ 重试过程也要通知；
> ④ 多人版只发汇总；⑤ 不需要远程控制，只接收。
>
> **v1.1 变更**（真机反馈）：Server酱 的正文键是 **`desp`** 不是 `body`（发 `body` 会「成功但正文空白」），
> `render()` 现按 `isServerChan()` 自动选键名；并新增 `businessError()` 识别**藏在 HTTP 200 里的业务失败**。
> 对应固件 `0.3.1`。

---

## 1. 为什么 BLE 不算「通知」

BLE 的 `emitEvent()` 只有在手机/PC 控制台**正在线**时才收得到，设备端不会为离线中心端补发。
断连即丢，且没有服务端重发语义。所以通知的实质是**给设备加一条独立的网络出口**：

```
事件源（打卡/体检/调度器） → NotifyService::post() → 去重 / 配额 / 渲染 → RAM 队列
                                    │                                      │
                                    └── BLE 镜像（控制台在线就能看到）      └→ pump() → HTTPS POST/GET → 推送服务 → 手机
```

BLE 镜像保留是为了调试方便，**不承担送达责任**。

---

## 2. 通道格式（`notifyStyle`）

一条 URL 就能接，不需要为每个服务写代码。`render()` 按 style 渲染：

| style | 形式 | 典型服务 | URL 示例（占位） |
|---|---|---|---|
| 0 | **GET 模板**，`{title}` / `{body}` 自动 URL 编码 | Bark（iOS）、Server酱（微信） | `https://api.day.app/<KEY>/{title}/{body}`<br>`https://sctapi.ftqq.com/<SENDKEY>.send?title={title}&desp={body}` |
| 1 | **POST 纯文本**，首行=标题 | ntfy（Android/iOS/网页） | `https://ntfy.sh/<你的主题名>` |
| 2 | **POST JSON**（默认） | 钉钉 / 企业微信 / 自建中转 | `https://oapi.dingtalk.com/robot/send?access_token=<TOKEN>`<br>或自带模板：`https://例.com/hook{"title":"{title}","text":"{body}"}` |

* style 2 若 URL 里没有 `{`，就发标准三字段 JSON：`{"title":…,"body"或"desp":…,"device":…}`；
* style 0 若 URL 里没有 `{`，退化成追加 `?title=…&body=…`；
* URL 里含 `{` 的 JSON 模板按**第一个 `{`** 切分：前面是真 URL，后面是模板（值做 JSON 转义）。
* **Server酱 特例（重要）**：它的正文字段叫 `desp`，且**不认 `body`**。不带 `{}` 模板时，固件按 URL 里的
  `ftqq.com` / `ft07.com` / `.send` 自动把正文键名换成 `desp`，style 1 的纯文本也会自动降级成
  `application/x-www-form-urlencoded` 的 `title=…&desp=…`。所以下面三种写法都能收到**有正文**的消息：
  ```
  notify set url=https://sctapi.ftqq.com/<SENDKEY>.send?title={title}&desp={body} style=0 on=1
  notify set url=https://sctapi.ftqq.com/<SENDKEY>.send style=0 on=1
  notify set url=https://sctapi.ftqq.com/<SENDKEY>.send style=2 on=1
  ```
  `notify status` 会回显 `contentField:"desp"` 表示当前用的是哪个键名。
* **失败也可能藏在 HTTP 200 里**：Server酱 / 钉钉 的拒收（SendKey 错、当天条数用尽、频率超限）照样返回
  `200`，只在 JSON 里写 `code≠0`（钉钉是 `errcode`）。`deliver()` 现在会用 `businessError()` 解析响应体，
  把 `code 40001 message…` 这类当成失败 → 进队列按 5 s / 30 s / 5 min / 10 min 退避重试，最多 4 次。
  *实测 2026-10-18：`{"title":…,"body":…}` 被 Server酱判为 `code:0` 发送成功，但手机上的正文是空的 ——
  这就是"通知收到了却看不到内容"的成因；同一 URL 改成 `desp` 后正文正常显示。*

**局域网中转（推荐给不想暴露公网 token 的人）**：在家里跑 `tools/notify_sink.py`（监听 `:8770/notify`，收到后转桌面提示 / 转发 Bark / ntfy），设备只把
`notify set url=http://192.168.x.x:8770/notify style=2 on=1` 指向它，token 全留在 PC 上，设备无需任何推送凭据。

---

## 3. 配置项

| 项 | NVS 键 | 默认 | 说明 |
|---|---|---|---|
| 开关 | `n_on` | `false` | 关着时 `post()` 静默返回，绝不影响打卡 |
| 格式 | `n_style` | `2` | 见上表 |
| 地址 | `n_url` | 空 | 含 token，日志/状态里**只显示脱敏形式** |
| 去重间隔 | `n_gap` | `60` s | 同 key 在间隔内只发一条 |
| 每日配额 | `n_quota` | `40` 条 | `0` = 不限 |

指令：`notify`（查配置+今日已发+队列）、`notify set url=… [style=…] [on=0|1] [gap=秒] [quota=条]`、
`notify test [文本]`（**同步立即发**，不入队、不受去重与配额限制——用它验证通道）。

---

## 4. 事件表

| NotifyKind | 标题 | 触发点 | 去重 key | 一天上限 |
|---|---|---|---|---|
| `StatusDigest` | 21:00 状态摘要 | 窗口起点调度器体检走 `workMultiCheckin`（`probeOnly` 且**来自调度器**才发；手动 `test.probe` 不占用当天这条；0 个生效也照发，摘要正文会说明「生效 0 人」） | 今天日期 | 1 |
| `CheckinOk` | 打卡成功 | 定时：全部生效账号成功，**一条汇总**（`<今天>/multi`）；手动 `test.checkin` 成功/今日已打卡（`<今天>/ok`） | `<今天>/multi`、`<今天>/ok` | 1 |
| `CheckinFail` | 打卡失败 | 定时：本轮未全成功（限流见 §5）；手动单账号本轮终态失败 | `<今天>/multi-fail/<桶>`（桶=(轮-1)/3）、`<今天>/fail` | 分桶 |
| `CheckinRetry` | 打卡重试 #N | kind 与「#N」自动编号标题保留；**现行固件把重试并入 CheckinFail 汇总**（正文自带「将在 3 分钟后重试」） | `<今天>/pending/<桶>` | 每 3 轮 1 条 |
| `DeadlineMissed` | 窗口已过仍未成功 | 23:30 调度器 | `<今天>/deadline` | 1 |
| `NeedMfa` | 需要验证码 | 预登录或打卡被二次验证拦住 | `<今天>/mfa` | 1 |
| `NetDown` | 网络不可用 | Wi-Fi 连接失败 | `net/<小时>` | 每小时 1 |
| `Info` | 通知 | `notify test` | 不去重 | — |

摘要正文由 `multiStatusDigest()` 生成，形如：
`21:00:12 生效2人，关闭1人，成功1人；A01=ok、A02=pending｜窗口21:00-23:30 打卡22:00`
——被单独关闭的账号不逐条列结果，只计入「关闭 K 人」；账号表为空时按设备自身账号（`auth` + `loc`）体检。

**打卡只发一条汇总**（用户明确要求）：`本轮 3/3 成功` / `本轮 2/3 成功；未成功：A01(pending)、A02(http_error)；将在 3 分钟后重试`；
有单独关闭的账号时追加`（另有 K 个已单独关闭）`，如 `本轮 3/3 成功（另有 1 个已单独关闭）`。
**全部账号都关闭或表为空（生效 0 人）时一条不发**，避免调度器每 180 s 推「本轮 0/0 成功」这种废话。不逐账号刷屏。

---

## 5. 限流：为什么不会刷屏

打卡重试每 `AppConst::kCheckinRetrySeconds`=180 s 一次，窗口内最多 8 轮。逐轮发通知会淹没手机，因此：

* `NotifyService::bumpAttempt()` 每轮**无条件**计数（被挡掉的轮次也计入，编号才等于真实轮次）；
* 调用方只在 `round % 3 == 1`（第 1、4、7… 轮）或**全成功**时发通知，正文用 `CheckinFail`/`CheckinOk` 汇总
  （key `<今天>/multi` 或 `<今天>/multi-fail/<桶>`）——`NotifyKind::CheckinRetry` 这个 kind 保留但**当前不再单独发**，
  重试进度写在同一句正文末尾「将在 3 分钟后重试」；
* `post()` 内部再做一层：同 key 在 `n_gap` 秒内去重、超过 `n_quota` 条/天直接丢弃；
* 最终失败由 `DeadlineMissed` 收尾，成功由 `CheckinOk` 收尾 —— **一夜最多 4~5 条**。

## 6. 队列与重试

* 队列：`kQueueCapacity=8` 条，RAM 环形，满则挤掉 `createdAtSec` 最旧的一条（**P0 不落 NVS，重启丢失**）；
* 每条 body ≤250 B、title ≤63 B、key ≤23 B；
* 退避 `kBackoffSeconds = {5, 30, 300, 600}` s，`kMaxAttempts=4`，用尽后丢弃并记日志；
* **断网时 `pump()` 立即返回**（`WiFi.status() != WL_CONNECTED`）：不发起尝试 ⇒ 不消耗重试次数、
  不推进退避，断网期间入队的通知在链路恢复后仍能补发（仍受 8 条容量与每日配额限制）；
* 发送超时 6 s，跟随重定向；
* 驱动点在 `CommandRouter::processPending()` 的 200 ms 队列唤醒上（`m_notify->pump()`）——
  **不新增任务、不新增定时器**，HTTP 只可能在 net_task 里发生。

## 7. 失败隔离（硬约束）

通知的任何异常都**不得**改变打卡结论：
`post()` 全程不返回错误码，只写日志；队列满、配额用尽、HTTP 4xx/5xx、TLS 失败——打卡流程与
`AppConfig::last` 都不受影响。`status` / `today` 查询永远是事实来源，通知只是投影。

## 8. 安全与隐私

* 只允许 `https://`（自建局域网中转可 `http://`，token 不出内网）；`tls_mode=0`（setInsecure）不是默认，
  且若被设置，`http_client` 日志会明确告警；
* 证书按 host 分流：`*.uestc.edu.cn` 仍只用校园侧抓到的 CA（`uestc_ca_pem.h`），公网推送域名用
  `public_ca_pem.h`（`tools/gen_public_ca.js` 生成，12 张非叶子证书 / 23,588 B；
  **只放自签根会验不过钉钉**，需要把 AIA 中间证书一并内置）；
  换/加推送域名后可用 `tools/verify_public_ca.js <host> [<host> …]` 只拿这份 PEM 做真实 TLS 握手复验；
* 推送 URL 含 token：日志与 `notify` 回执只输出 `scheme://host/***(len=N)`；
* 正文不含学号、密码、SESSION、验证码、精确坐标；多人汇总只用账号标签（≤12 字符的自定义字符串）。

## 9. 与调度器的关系

21:00 的摘要由已有的 `Trigger::Probe` 顺路产生，22:00 的打卡由 `Trigger::Checkin` 产生结果通知，
23:30 的告警由 `Trigger::DeadlineMissed` → `WorkKind::DeadlineMissed`（必须走 net_task 才能发 HTTP）。
通知不引入任何新的定时逻辑，因此不存在「通知把设备叫醒」这类副作用。

## 10. 已知限制 / 后续

| 项 | 状态 |
|---|---|
| 队列持久化到 NVS（重启后补发） | P1 未做 |
| `NetDown` 只覆盖显式连接失败；自动重连期间的失败还没有挂点 | P1 |
| 短信/邮件通道 | 不做（依赖重、Flash 紧张） |
| 经典 ESP32（4 MB 分区）Flash 占用 | ✅ 已解决：分区表改**方案 B**（app0/app1 各 `0x1F0000`=2,031,616 B，保留 OTA），现占 68.0%（1,381,353 B）；改分区表需整片重刷含 `partitions.bin` |
