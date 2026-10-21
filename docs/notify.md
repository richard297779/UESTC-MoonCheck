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
