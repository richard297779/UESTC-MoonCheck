# 快速开始

> 本文假设板子**已经刷好固件**。还没刷？先看 [刷固件](flashing.md)。
>
> 全程约 10 分钟，只需要：一块板子、一根 USB 数据线、一台电脑（Chrome / Edge）、Python 3.10+。

---

## 准备清单

| 项目 | 说明 |
| --- | --- |
| 硬件 | 已烧录固件的板子、USB 数据线、5 V 供电 |
| 软件 | Python 3.10+（推荐 3.12）+ `bleak` |
| 信息 | 学号、统一身份认证密码、微信授权凭据、宿舍 WiFi 名与密码 |
| 现场 | **浏览器指纹**（CAS 登录用）、**宿舍坐标**（必须在宿舍取） |

> GitHub Releases 中已发布的 v0.4.0 预编译固件早于微信票据更新。当前源码版本为 `0.5.1`；在 v0.5.1 Release 发布前，请按[自行编译](../firmware/README.md)从当前源码构建。

---

## 1. 安装 bleak

```powershell
python --version                 # 需要 3.10+
python -m pip install bleak
```

用 conda 的话：

```powershell
conda create -n WANAN python=3.12 -y
conda activate WANAN
python -m pip install bleak
```

> 以后每次连接设备前，先 `conda activate WANAN`。

---

## 2. 准备授权信息

### 2.1 取「浏览器指纹」

这个值决定设备登录时会不会被要求短信验证码。**必须用你平时打卡、且登录过的那个浏览器。**

1. 用 Chrome 打开 <https://jzsz.uestc.edu.cn/>，正常登录统一身份认证。
   如果出现二次验证，勾选「信任此浏览器」再完成。

   ![登录统一身份认证](assets/quickstart/03-login-jzsz.png)

2. 按 **F12** → **Application（应用程序）** → 左侧 **Cookies** → 点开 `https://idas.uestc.edu.cn`。
3. 找到 **`MULTIFACTOR_BROWSER_FINGERPRINT`**，复制它的值（32 位十六进制）。

   ![在 Cookies 里找到指纹值](assets/quickstart/04-cookie-fingerprint.png)

> 它是 **HttpOnly**，在 Console 里执行 `document.cookie` 看不到，必须用 Application 面板。
>
> 备用取法：F12 → **Network** → 过滤 `bfp` → 找 `GET /authserver/bfp/info?bfp=…`，URL 里 `bfp=` 后面就是它。

![备用取法：bfp/info 请求](assets/quickstart/05-bfp-info-url.png)

> 🔒 这个值等于「免验证码的钥匙」，不要发群、不要截图、不要上传网盘或 GitHub。

### 2.2 准备微信授权凭据

新版固件在提交打卡前，会使用微信小程序授权链向平台换取短效操作票据。每个账号需要本人授权得到的 `openid`、`access`、`refresh` 和 `expiresAt`（Unix 时间戳，秒）。

1. 在电脑浏览器打开以下 QR Connect 页面。核对地址栏主机名为 `open.weixin.qq.com` 且连接为 HTTPS：

   ```text
   https://open.weixin.qq.com/connect/qrconnect?appid=wxd44977328b36e647&redirect_uri=https%3A%2F%2Fyybadaccess.3g.qq.com%2Fpc_yyb%2Fpcyyb_oauth%3Flogin_type%3DWX&response_type=code&scope=snsapi_login,snsapi_runtime_pcsdk&state=web&fast_login=1&self_redirect=true
   ```

2. 用**将用于打卡的微信账号本人**扫码并在微信中确认授权。每个打卡账号都需分别授权一次。
3. 授权完成后，在该页面按 **F12** 打开 Chrome DevTools，选择 **Application（应用程序）→ Cookies**，在左侧展开框架/来源，选中 `static.pc.yyb.qq.com`。协议可能显示为 `http://` 或 `https://`，以 DevTools 中该域名实际列出的来源为准；不要选顶层页面或其他域名下的 Cookie。
4. 在右侧 Cookie 表中分别找到并复制 `openid`、`accesstoken`、`refreshtoken`、`expires_in` 四项的 **Value**。不要把值截图、发给他人或粘贴到不可信网页。上游 [MoonCheck-WX 快速开始](https://github.com/muyuhuanghun/MoonCheck-WX#快速开始)介绍了 QR Connect 凭据的用途；更多技术背景见上游[技术报告](https://github.com/muyuhuanghun/MoonCheck-WX/blob/main/docs/TECHNICAL-REPORT.md)。

**导入到开发板前，先关闭完成扫码授权的 QR Connect 页面/标签页。** 同一组微信授权凭据应始终只由一个客户端保活；不要让扫码页面、其他电脑程序和开发板同时使用或刷新这组凭据。多人配置时，每个账号依次单独扫码、复制凭据并关闭对应页面，再导入该账号的槽位。

字段对应关系：

| Cookie 名 | `wx.import` 字段 | 说明 |
| --- | --- | --- |
| `openid` | `openid` | 微信用户标识 |
| `accesstoken` | `access` | 访问令牌 |
| `refreshtoken` | `refresh` | 刷新令牌；可能轮换，务必保密 |
| `expires_in` | 用于计算 `expiresAt` | 剩余有效秒数，不是 Unix 时间戳，**不可直接照抄** |

固件要求 `expiresAt` 是 Unix 绝对时间（秒），所以将当前 Unix 时间加上 `expires_in`。例如 Cookie 值为 `7200` 时，在 PowerShell 中运行以下命令，将 `7200` 替换成你读到的值，再把输出填入 `expiresAt`：

```powershell
[DateTimeOffset]::UtcNow.ToUnixTimeSeconds() + 7200
```

如果不方便换算，也可先填 `0`；固件会在首次使用前刷新凭据并保存服务端返回的有效期和轮换后的令牌。

示例导入（把占位符替换为本人 Cookie 值；`expiresAt` 使用上一步算出的 Unix 秒）：

```text
wx import 0 <openid> <accesstoken> <refreshtoken> 0
wx status
wx test 0
```

`wx.test` 会测试刷新/换票链路，但不会提交打卡。成功后固件会将刷新后的凭据保存在设备 NVS 中；refresh token 可能轮换，之后继续由这块开发板使用设备内保存的新值，不要让已关闭的扫码页面或其他客户端再次保活/刷新旧值，也不要重复导入旧值。若刷新被拒绝，需由账号本人重新扫码授权并导入。微信令牌等同登录凭据：仅授权给本人设备、妥善保管；设备遗失时在应用宝授权管理中撤销授权。不要分享、截图、上传网盘或提交到 Git。

---

## 3. 取宿舍坐标

1. **人在宿舍**，用 Chrome 打开晚安成电页面：<https://jzsz.uestc.edu.cn/daily-checkin-h5/index.html?appId=294>。
   登录状态有效时，页面会显示地图和「请打卡」。

   ![晚安成电打卡页](assets/quickstart/06-h5-page.png)

2. 按 **F12** → **Network** → 过滤框输入 `reverse`。
3. 找到形如 `reverse?lat=30.7xxxxx&lng=103.9xxxxx` 的请求，从 URL 里读 `lat` 和 `lng`。

   ![从 reverse 请求读取经纬度](assets/quickstart/07-reverse-coords.png)

> ⚠️ 三个注意点：
>
> 1. **必须在宿舍现场取**——打卡围栏只有 130 米，人在别处取的坐标打不上。
> 2. 浏览器定位靠 WiFi 估算，**可能漂移**。刷新页面多取 2–3 次，取出现次数最多的那组；
>    也可以粘到高德地图的坐标拾取器，看落点是否在宿舍楼附近。
> 3. 顺序是 `lat,lng`（纬度在前），别写反。

---

## 4. 连接设备

```powershell
cd <仓库目录>
python tools/ble_console.py --list           # 看广播名，格式 WANAN-XXXX
python tools/ble_console.py --name WANAN-XXXX
```

连上后会提示「已订阅通知」。先输入 `hello` 确认通信正常（应返回 `fw`、`mac`、`flashMB`）。

![扫描并连接设备](assets/quickstart/08-ble-connect.png)

> 多块板同时上电时，`--list` 会显示多个 `WANAN-XXXX`，用 `--name` 指定要连的那块。

---

## 5. 写入配置

依次输入下面 6 条（每行一条）：

```text
wifi <宿舍WiFi名> <WiFi密码>
auth <学号> <密码> <第 2.1 步取到的指纹>
wx import 0 <openid Cookie> <accesstoken Cookie> <refreshtoken Cookie> 0
loc <纬度> <经度> 四川省成都市郫都区
sched 22:00 23:30 30
auto on
```

示例（**数值全是占位，请换成你自己的**）：

```text
wifi Dorm-2.4G mywifi123
auth 2023XXXXXXX ************ 0123456789ABCDEF0123456789ABCDEF
wx import 0 <openid> <accesstoken> <refreshtoken> 0
loc 30.753456 103.934123 四川省成都市郫都区
sched 22:00 23:30 30
auto on
```

| 项 | 注意 |
| --- | --- |
| WiFi | **必须 2.4 GHz**，板子不支持 5 GHz。不确定先输 `scan`，看你的 SSID 的 `channel` 是否在 1–13 |
| WiFi 名或密码含空格 | 用原始指令：`raw {"op":"wifi.set","ssid":"我的 WiFi","pass":"a b#c"}` |
| 指纹 | 用于 CAS 登录；如未配置或失效，登录仍可能要求短信验证码 |
| 微信凭据 | `expires_in` 是剩余秒数，须加当前 Unix 时间换算为 `expiresAt`，或填 `0` 由固件首次使用时刷新；用 `wx status` 查看槽位摘要，不会回显凭据明文 |
| `loc` 第三个参数 | 地址文本，可随意填，只影响打卡记录里显示的地点 |
| `sched` | 依次为：打卡时间、窗口结束、随机抖动秒数 |

`wifi` 成功后会自动校时，回执类似：

```json
{"evt":"ok","msg":"wifi connected","data":{"ip":"192.168.1.xxx","rssi":-52,"timeSynced":true}}
```

再输入 `time`，确认设备时间是当前北京时间。

---

## 6. 验证链路

### 6.1 全链路自检

```text
chain 0
```

期望回执（关键字段）：

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

| 现象 | 含义 | 怎么办 |
| --- | --- | --- |
| `appEntry:true` + `inWindow:false` | ✅ 一切正常，只是不在打卡时段 | 等 21:00–23:30 |
| `canCheckin:true` | ✅ 现在就能打 | 直接 `checkin` |
| `loginPage:false` | 网络 / DNS / TLS 不通 | `status` 看网络状态，再 `netcheck` |
| `session:false` + `loginDeferred:true` | 窗口外默认不登录（保护会话） | 用 `chain 0 force` |

![chain 0 验证通过](assets/quickstart/10-chain-ok.png)

`chain` 不检查微信票据。另用 `wx status` 确认凭据已导入，并运行 `wx test 0`（多人时按对应槽位调整）；若测试失败，查看错误回执和 `logs`，凭据刷新被拒绝时由账号本人重新扫码并导入。

### 6.2 手动打一次卡

仅在 **21:00–23:30** 内：

```text
checkin
```

```json
{"evt":"ok","msg":"checked in at 21:36:01 (distance 42.5 m)"}
```

| 返回 | 含义 | 处理 |
| --- | --- | --- |
| `ok:true … checked in at …` | ✅ 成功 | 完成 |
| `already checked in` | 今天已经打过 | 正常，不会重复提交 |
| `OUT_OF_WINDOW` | 不在 21:00–23:30 | 到点再试 |
| `NEED_MFA` | 服务端要验证码 | 指纹不对，回第 2 步重新取 |
| `wx ticket failed` / 缺少微信凭据 | 未能取得打卡操作票据 | 按账号槽位用 `wx import` 导入微信凭据，执行 `wx test 0` 验证；`chain` 不会检查票据 |
| `APP_UNAVAILABLE` / `pending` | 会话未授权 | 等设备自动重试，或 `login` 后再 `checkin` |
| `distance` 很大 / 记录异常 | 坐标不对 | 回第 3 步重新取坐标，再 `loc` |

### 6.3 收尾确认

```text
status
```

看到 `last.ok:true` 和正确的 `nextRun` 就算配置完成。之后每天 21:00 预登录、22:00 自动打卡，失败每 3 分钟重试到 23:30。

---

## 7. 常见问题

| 现象 | 处理 |
| --- | --- |
| `--list` 看不到设备 | 重新上电；关闭手机上占用设备的 BLE 工具；重开电脑蓝牙 |
| 连接超时 / GATT 报错 | 设置 → 蓝牙 → 删除设备后重连；把板子靠近电脑；换手机 nRF Connect 验证 |
| `wifi` 一直失败 | 确认 2.4 GHz、密码正确；名字含空格用 `raw` |
| `netcheck` 返回 302 / HTML | 校园网需要网页认证，换不需要认证的网络（路由器 / 手机热点） |
| `NEED_MFA` | 指纹没填或填错，重新按第 2 步取 |
| 白天 `inWindow:false` | 正常，打卡窗口是 21:00–23:30 |
| 想重置 | `clear` 清会话；`reset` 恢复出厂（会二次确认） |

更多见 **[常见问题](faq.md)**。

---

## 8. 安全提醒

- 学号密码、浏览器指纹和微信授权凭据都属于敏感凭据。只导入本人账号并妥善保管，不要发群或上传网盘 / GitHub。
- 凭据保存在设备 NVS；`status` 输出会脱敏学号，密码和指纹只报是否已设置。微信凭据摘要可用 `wx status` 查看，不回显令牌。
- 设备只调用学校打卡接口，不要拿它做其他用途。
- 把板子放在宿舍 WiFi 信号好的位置长期通电即可，不需要移动。

---

## 9. 常用命令

| 命令 | 作用 |
| --- | --- |
| `hello` / `status` / `time` | 版本与 MAC / 全量状态 / 设备时间 |
| `wifi <ssid> [pass]` | 配置 WiFi |
| `scan` / `netcheck` | 扫描 2.4 GHz / 判断是否需要网页认证 |
| `auth <学号> <密码> [指纹]` | 配置账号 |
| `loc <lat> <lng> [地址]` | 配置坐标 |
| `sched <打卡时间> [结束] [抖动]` | 配置时间 |
| `auto on\|off` | 开关每日自动打卡 |
| `login` / `probe` / `chain` / `checkin` | 登录 / 只读体检 / 全链路自检 / 打卡 |
| `logs 30` | 查看最近 30 行日志 |
| `account …` / `multi …` | 多账号管理 |
| `notify …` | 手机通知 |
| `quit` | 退出（配置不会丢） |

完整说明见 **[指令手册](commands.md)**。
