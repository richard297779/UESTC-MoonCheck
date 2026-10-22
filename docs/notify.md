# 手机通知

打卡结果可以推送到手机。**通知只是附加功能**，推送失败不影响打卡本身。

支持四种通道：

| 通道 | 适合 | `style` |
| --- | --- | --- |
| Bark（iOS） | 最省事，有官方 App | `0` |
| ntfy | 安卓 / iOS / 网页都行，可自建 | `1` |
| Server酱 | 微信接收 | `2` |
| 局域网中转 | 不想把推送 token 存在设备里 | `2` |

---

## Bark（iOS，推荐）

1. App Store 安装 **Bark**，打开后复制你的推送地址，形如
   `https://api.day.app/AbCdEf123456/`。
2. 配置设备：

```text
notify set url=https://api.day.app/AbCdEf123456/{title}/{body} style=0
notify test 测试一下
```

手机应立刻收到「测试一下」。

> `{title}` / `{body}` 是占位符，设备会把标题和正文填进去。`style=0` 表示按 GET 模板发送。

---

## ntfy

1. 装 **ntfy** App，或在浏览器打开 <https://ntfy.sh/>。
2. 想一个别人猜不到的主题名，例如 `wanan-7f3k9x2m`，在 App 里订阅它。
3. 配置设备：

```text
notify set url=https://ntfy.sh/wanan-7f3k9x2m style=1
notify test 测试一下
```

> `style=1` 表示纯文本 POST，首行是标题。主题名等于密码，别用 `test`、`wanan` 这种。

自建 ntfy 服务器就把域名换掉即可。

---

## Server酱

1. 打开 <https://sct.ftqq.com/>，微信扫码登录，复制 **SendKey**。
2. 配置设备：

```text
notify set url=https://sctapi.ftqq.com/<你的SendKey>.send style=2
notify test 测试一下
```

> `style=2` 是 JSON POST。Server酱的正文键名是 **`desp`**（不是 `body`/`content`），
> 0.3.1 固件会自动识别，无需手动处理。

如果微信只收到标题、正文是空的，说明发的是旧格式，升级固件到 0.3.1 以上。

---

## 局域网中转（进阶）

不想把推送 token 写进设备？让设备把通知发到宿舍里的一台电脑，由电脑转发。

**PC 侧**（需要保持开机，纯 Python 标准库）：

```powershell
# 只打印到终端 + 提示音
python tools/notify_sink.py

# 收到后转发给 Bark
python tools/notify_sink.py --forward https://api.day.app/<KEY>/{title}/{body}

# 转发给 ntfy
python tools/notify_sink.py --forward https://ntfy.sh/<主题名>
```

默认监听 `8770` 端口。查看本机局域网 IP（`ipconfig`），然后配置设备：

```text
notify set url=http://192.168.1.50:8770/notify style=2
notify test 测试一下
```

> 电脑 IP 变了就要重新配置。建议在路由器上给这台电脑绑定固定 IP。

---

## 什么情况下会收到通知

| 事件 | 说明 |
| --- | --- |
| 21:00 预登录结果 | 登录成功 / 失败摘要 |
| 打卡成功 | 含时间与距离 |
| 打卡失败 | 含错误码与原因 |
| 重试 | 每 3 轮发 1 条，避免刷屏（默认 `gap=180` 秒、`quota=20` 条） |
| 窗口结束仍未成功 | 23:30 发一条总结 |
| 需要验证码（`NEED_MFA`） | 指纹失效，需要重新取指纹 |
| 网络不可用 | 连不上 WiFi / 校园网未认证 |

---

## 限流与参数

```text
notify set url=<地址> style=<0|1|2> on=1 gap=180 quota=20
```

| 参数 | 含义 | 默认 |
| --- | --- | --- |
| `on` | `1` 开启，`0` 关闭（保留地址） | `1` |
| `gap` | 两条通知之间最少间隔（秒） | `180` |
| `quota` | 每天最多发送条数 | `20` |

`notify test` 立即发送、不入队、不占配额，用来验证地址是否配通。

查当前配置：`notify` 或 `status`。

---

## 多通道分发（0.4.0 起）

一台设备最多 **6 路通道**：1 路默认通道（上面 `notify set` 配的那条）+ 5 路附加通道。
用途：**每个人的打卡结果单独推给每个人**（各自的 Server酱 / Bark / ntfy）。

```text
notify add A02url url=https://sctapi.ftqq.com/<第二个SendKey>.send style=2 accounts=A02
notify link A02url A02,A03      # 改关联（或 all = 收所有账号）
notify off A02url               # 单独停用这条（不删地址）
notify rm A02url                # 删除
notify test chan=A02url 单独试这条   # 不带 chan= 仍走默认通道
```

分发规则（和桌面版完全一致）：

| 事件 | 默认通道 | 附加通道 |
| --- | --- | --- |
| 单账号打卡结果、登录摘要、断网等全局事件 | ✅ | `accounts` 含 `all` 的也收 |
| 多人轮：逐账号结果 | ✅ | 只发给**关联该账号**的通道 |
| 多人轮：整轮汇总 | ✅ | ❌（避免每人重复） |

- 每人一天只推一条「打卡成功」（去重按 账号+日期+类型）；失败按每 3 轮一条的节奏。
- 配额 `gap`/`quota` 对**每条通道独立**计数；`notify` 列表里每通道显示自己的今日已发数。
- 通道 label 规则同账号标签（`A-Za-z0-9-_`，≤12 字符），列表/`status` 里 url 一律脱敏。
- 「通知只是附加功能」对每条通道同样成立：某路挂了不影响其他路，也不影响打卡。

---

## 通知排错

| 现象 | 处理 |
| --- | --- |
| `notify test` 没反应 | 先 `http <你的推送地址>` 看设备网络是否通 |
| 返回 401 / 403 | token / SendKey 错了，重新复制 |
| 返回 404 | URL 拼错；Bark 要保留 `/{title}/{body}` 占位符 |
| 微信只有标题没正文 | 升级到 0.3.1+，Server酱正文键是 `desp` |
| 每天只收到一两条 | 触发了 `gap` / `quota` 限流；重试通知每 3 轮才发 1 条 |
| 重启后丢了几条 | 通知队列在内存里，掉电会丢；不影响打卡结果 |
| 局域网中转收不到 | 确认 PC 防火墙放行 8770，且设备与 PC 在同一网段 |

> 所有通知发送失败都只在日志里记一行，**不会影响打卡流程**，也不会阻塞下一条。
