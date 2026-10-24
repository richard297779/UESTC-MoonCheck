# PC 验证工具（可选）

在动手配设备之前，先用电脑把「登录 → 门户预热 → 打卡」整条链路跑通，
可以提前发现账号、密码、指纹、坐标的问题，避免反复折腾板子。

> 这里只是**可选的前置验证**。不想用也可以直接按 [快速开始](../../../docs/quickstart.md) 配置设备。
>
> **历史工具说明：** 此 PC 参考实现对应旧打卡协议，尚未实现平台于 2026-10-23 起要求的微信 `X-Checkin-Ticket`。它不能验证当前完整打卡链路；请用更新后的固件执行 `wx.test` 与 `chain` 分别验证。

---

## 准备

```powershell
# 1) 确认 Python
python --version        # 需要 3.10+

# 2) 复制配置模板
cd tools\pc_reference
copy config.example.json config.json
notepad config.json
```

`config.json` 里需要填：

| 字段 | 说明 |
| --- | --- |
| `student_id` | 学号 |
| `password` | 统一身份认证密码 |
| `session_cookie` | 浏览器里 `SESSION` 的值（可选，留空则脚本自己登录） |
| `browser_fingerprint` | 32 位浏览器指纹（取法见 [快速开始](../../../docs/quickstart.md#2-取浏览器指纹)） |
| `lat` / `lng` | 宿舍坐标 |

> 从浏览器 F12 → Application → Cookies → `jzsz.uestc.edu.cn` 可以拿到 `SESSION`。
> 不填也能用：脚本会用学号密码自己登录，但需要 `pycryptodome`。

---

## 常用命令

```powershell
# 自检（不联网），确认环境没问题
python verify.py selftest

# 只读：看会话、今日状态、打卡窗口、打卡点（零第三方依赖）
python verify.py status

# 干跑：打印将要提交的载荷，不真的打卡
python verify.py checkin --dry-run

# 真实打卡（每天只能一次，21:00–23:30 之间）
python verify.py checkin

# 脱离浏览器登录（需要 pycryptodome）
python -m pip install pycryptodome
python verify.py login

# 只读体检平台会话是否被授权
python verify.py probe-session
```

`status` 里重点看：`sessionValid`、`recordStatusCode`（`PENDING` = 今天还没打）、
`inWindow`、`distanceMeter`（应在 130 米内）。

---

## 文件说明

| 文件 | 作用 |
| --- | --- |
| `verify.py` | 命令行入口 |
| `wsc_http.py` | 最小 HTTP 客户端（手工 Cookie、手工跟随 302，零依赖） |
| `wsc_cas.py` | 统一身份认证登录 + 密码加密 |
| `wsc_checkin.py` | 晚安成电接口封装 |
| `config.example.json` | 配置模板 |

---

## 安全约定

- `config.json` 里有密码和会话 Cookie，**等同账号通行证**，已被 `.gitignore` 忽略，不要外传或提交。
- 脚本不会打印密码、Cookie、姓名和手机号；`status` 只输出必要字段。
- 频率纪律：登录实验每天 ≤ 3 次，打卡每天 1 次，只读探测 ≤ 5 次。
- 一旦出现滑块、验证码或 `abnormalFlag=true`，**立即停止**，不要连续重试。
- 验证完成后建议清空 `config.json` 里的密码。
