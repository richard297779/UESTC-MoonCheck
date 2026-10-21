# 抓包操作指南（Burp Suite + 浏览器 DevTools）

> 对应 README §13 的 C1–C6。**这是本项目的 P0 关键路径**：抓不到的东西，设备侧就实现不了。
> 目标读者：有电脑操作能力、但没做过 HTTPS 抓包的开发者。按本文顺序执行即可。
>
> **v1.1 修订**：并入 10-11 现场实测的两条发现（见 §0.2），据此重排了 Stage 4 与 Stage 4′。

---

## 0. TL;DR（六步最短路径）

1. 用 Burp Suite Community 打开 Proxy（监听 `127.0.0.1:8080`），Intercept **保持关闭**；
2. 装 Burp CA 到 Windows「受信任的根证书颁发机构」；
3. 用一个**全新的 Chrome/Edge 用户配置**（独立 `--user-data-dir`）并把它指向 Burp 代理；
4. 在这个干净浏览器里，完整走一遍：登录 →（若出现）二次验证 → 进平台 → 点「晚安成电」→ **在新窗口里**点击打卡；
5. 期间 **在每个标签页都开 DevTools**（Network / Preserve log），同时让 Burp 全程记录；
6. 导出「Burp XML + DevTools HAR + 关键请求的 Copy as cURL」，按 §6 脱敏后交付。

**预计耗时**：首次配置环境 20–30 分钟 + 走流程 15 分钟 + 导出脱敏 20 分钟。

---

## 0.2 ⚠️ 现场实测的两条发现与应对（**务必先读**）

### 发现 1：干净浏览器里登录，**没有**出现二次验证

**这说明什么**：免二次验证的判定**不（只）依赖浏览器本地 Cookie**。既然全新配置（无任何 Cookie）都没被要求验证码，那它很可能基于下列之一：

- 账号 + **常用设备指纹**（UA 等）判定；
- 账号 + **常用网络 / IP** 判定；
- **周期性策略**（如"同一环境 N 天内免验证"，平台的"7 天免登录"文案值得警惕）；
- 该账号当前处于人工放宽状态。

**对项目的影响（重要，也是好消息）**：

| 影响 | 说明 |
| --- | --- |
| 关键路径变简单 | 若长期不触发，设备只需"账号 + 密码"就能登录，**F2（验证码中继）从关键路径降级为兜底能力**（信任到期、换网络、风控时仍会用上） |
| 新增必做实验 | 必须查清"**什么条件下才会触发**"——这决定设备多久需要你人工介入一次。见 **Stage 3′** 矩阵 |
| 设计澄清 | 抓包时必须逐条记录 `Set-Cookie`：**如果登录成功的响应里根本没有"信任类"长效 Cookie**，就进一步印证"免验证记录在服务端而非浏览器" |

> ⚠️ 推论中最关键的一条：如果触发条件是 **UA（+IP）**，那设备只要**逐字固定同一个 User-Agent**，就等价于那台"被信任的浏览器"——这是最理想的结果。所以 **Stage 3′ 的 B4（改 UA）是全场价值最高的一个实验**。

### 发现 2：点击「晚安成电」会**打开新的标签页/窗口**

**这是个陷阱**：

| 风险 | 后果 |
| --- | --- |
| **DevTools 的 Network 面板只记录它自己所在的那个标签页** | 只盯原标签页 → **打卡请求一条都抓不到**，今晚白干 |
| 新页面可能是**不同域名**（独立微应用） | 设备要维护**多个域**的 Cookie；会话范围、请求头、CORS 都不同 |
| 新页面的 URL 里可能带**一次性启动凭据**（`token=`/`ticket=`/`code=`），或由原页面的 XHR 先取一个"微应用地址"再 `window.open` | 设备必须先复现"进入应用"这一步，否则打不了卡 |

**三条操作纪律（今晚必须遵守）**：

1. **以 Burp 为准**：Burp 的 HTTP history 会记录**所有标签页/窗口**的请求（只要走同一代理），是唯一不会漏的地方；
2. **每开一个新标签页/新窗口，先按 F12 打开 DevTools**，同样勾选 `Preserve log` + `Disable cache`，然后再操作；
3. 在**原标签页**里，点击图标前后注意 `Fetch/XHR` 中"取应用地址"的请求 → 右键 `Copy as cURL` 保存。

---

## 0.3 今晚的执行清单（照着做）

| # | 动作 | 完成 |
| --- | --- | --- |
| 1 | 启动 Burp，确认 `127.0.0.1:8080` 监听、Intercept 关闭 | ☐ |
| 2 | 确认 Burp CA 已装进 Windows 根存储，`https://idas.uestc.edu.cn` 不报证书错误 | ☐ |
| 3 | 确认 QUIC 已禁用（`edge://flags/#enable-quic`） | ☐ |
| 4 | 用独立配置启动专用浏览器，打开 DevTools（Preserve log / Disable cache） | ☐ |
| 5 | 清空该浏览器站点数据 + 清空 Burp history + 清空 DevTools | ☐ |
| 6 | 走 Stage 1–2（登录页 → 提交登录），**逐条标注**请求 | ☐ |
| 7 | 若出现二次验证 → 走 Stage 3；**无论是否出现** → 走 Stage 3′ 矩阵（至少做 B4） | ☐ |
| 8 | Stage 4 进平台；**点击「晚安成电」后立刻给新窗口开 F12** | ☐ |
| 9 | Stage 4′ 记录新窗口 URL 与启动参数 | ☐ |
| 10 | Stage 5 点击打卡（在**新窗口**里），抓 `#7-checkin-POST` | ☐ |
| 11 | Stage 6 定位来源观察 | ☐ |
| 12 | Stage 7/8 信任与生命周期实验 | ☐ |
| 13 | Stage 9 校园网认证（`netsh` + Portal 抓包） | ☐ |
| 14 | 导出 + 脱敏 + 填自检清单（§7） | ☐ |

---

## 1. 产出物清单（缺一不可）

| # | 产出物 | 用途 |
| --- | --- | --- |
| P1 | `capture/raw/uestc-login.har`（DevTools 导出，含内容，**每个标签页各导一份**） | 最完整的原始记录 |
| P2 | `capture/raw/burp-items.xml`（Burp 导出，**跨标签页全覆盖**） | 逐条查看/重放 |
| P3 | 关键请求的 **Copy as cURL**（脱敏后） | 最快定位字段名与请求头 |
| P4 | `docs/capture-notes.md`（按 §4 模板填写） | 人工判读结论 |
| P5 | 判决性实验结果（Stage 7/8） | 决定"长期无人值守"能否成立 |
| P6 | `idas_login_page.html` / `login.js` 原文 | 密码加密算法的唯一依据 |
| P7 | 校园网认证抓包（若为 Portal） | 决定设备能否上网 |
| **P8** | **新窗口（微应用）的完整 URL + 是否含一次性启动凭据 + 它所在的域名** | 决定"进入应用"这一步能否复现 |
| **P9** | **Stage 3′ 矩阵结果（MFA 触发条件）** | 决定多久需要人工介入一次 |

---

## 2. 环境准备

### 2.1 Burp Suite

| 版本 | 说明 | 建议 |
| --- | --- | --- |
| Community（免费） | 够用；**不能保存工程文件**，关闭前必须导出历史 | ✅ 本项目够用 |
| Pro（收费） | 可存工程、Logger 更完整 | 有就用 |

启动后确认：

- `Proxy → Proxy settings → Proxy listeners`：存在 `127.0.0.1:8080`（无则 Add）；
- `Proxy → Intercept`：**关闭拦截**（否则页面会卡死）；
- `Proxy → Proxy settings → HTTP`：`Allow HTTP/2` 默认开启；若网站异常，改为关闭再试；
- `Settings → Tools → Logger`：开启。

> **今晚的核心纪律**：Burp 是你唯一能跨标签页看全链路的工具。DevTools 只作补充。

### 2.2 准备一个"干净"的专用浏览器配置

```powershell
# Chrome
& "C:\Program Files\Google\Chrome\Application\chrome.exe" `
  --user-data-dir="$env:USERPROFILE\burp-profile" `
  --proxy-server="http://127.0.0.1:8080" `
  --no-first-run --no-default-browser-check

# 或 Edge
& "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" `
  --user-data-dir="$env:USERPROFILE\burp-profile-edge" `
  --proxy-server="http://127.0.0.1:8080" `
  --no-first-run --no-default-browser-check
```

**替代方案**：不装 CA 时可用 `--ignore-certificate-errors` 应急；但 Stage 7/8 的信任实验建议在"正常信任 CA"的环境下做，更贴近真实浏览器。

### 2.3 安装 Burp CA 证书到 Windows 根存储

1. 专用浏览器里访问 **`http://burp`** → 右上角 **CA Certificate** → 下载 `cacert.der`；
2. **以管理员身份**打开 PowerShell：
   ```powershell
   certutil -addstore -f "ROOT" "$env:USERPROFILE\Downloads\cacert.der"
   ```
3. 图形化等价：`certmgr.msc` → 「受信任的根证书颁发机构」→「证书」右键 →「所有任务」→「导入」；
4. **完全关闭并重启专用浏览器窗口**；
5. 验证：访问 `https://idas.uestc.edu.cn` 不再报证书错误；
6. 实验结束后如不放心，可在 `certmgr.msc` 中删除 `PortSwigger CA`。

### 2.4 关闭 QUIC / HTTP3（必须做）

Chrome：`chrome://flags/#enable-quic` → **Disabled** → 重启。
Edge：`edge://flags/#enable-quic` → **Disabled** → 重启。

### 2.5 浏览器内必要设置

| 设置 | 位置 | 作用 |
| --- | --- | --- |
| 允许位置权限 | 地址栏左侧图标 → 位置 → 允许 | 否则打卡页拿不到定位，按钮不会变 |
| Preserve log | F12 → Network → 勾选 | 跳转后不丢请求 |
| Disable cache | F12 → Network → 勾选 | 保证拿到完整响应 |
| 关闭"安全 DNS" | 设置 → 隐私 → 关闭"使用安全 DNS" | 避免对照实验失真 |
| **每个新标签页都开 F12** | — | **发现 2 的应对，最重要** |

---

## 3. 抓包流程（分幕执行）

> **通用要求**：每抓到一个"关键请求"，立刻在 Burp 里右键 → `Highlight` 选色，并在 `Comment` 里写编号（如 `#7-checkin`）。**边抓边标注**。

### Stage 0 · 建立干净基线

1. 专用浏览器清空所有站点数据（时间范围选"时间不限"）；
2. Burp `Proxy → HTTP history` → `Clear history`；
3. DevTools → Network → 清空，勾 Preserve log / Disable cache。

### Stage 1 · CAS 登录页

| 要做的事 | 访问 `https://jzsz.uestc.edu.cn/`，观察跳转到 `idas.uestc.edu.cn/authserver/login?...` |
| --- | --- |
| 必须抓到 | ① 跳转链（302 的 `Location`，注意 `service=` 参数）② 登录页 HTML ③ `login.js`（**完整内容**） |
| 保存 | 登录页 → 查看源代码 → 另存 `capture/raw/idas_login_page.html`；Sources 里 `login.js` → 右键 `Save as` |
| 判读 | 搜索 `encryptPassword`、`pwdDefaultEncryptSalt`、`AES`、`CryptoJS` —— 密码加密算法的唯一依据 |

### Stage 2 · 提交登录（关键）

| 要做的事 | 输入学号 + 密码，提交 |
| --- | --- |
| 必须抓到 | `POST /authserver/login`（或类似），记为 `#2-login-POST` |
| 记录重点 | 请求体全部字段（`username / password / execution / _eventId / lt / cllt / dllt / rmShown`…）；`username` 是否明文；`password` 形式（Base64？长度）；请求头 `Content-Type / Referer / Origin / User-Agent`；响应状态码与**全部 `Set-Cookie`** |
| **特别注意** | **本次大概率不会跳二次验证**（发现 1）。请明确记录响应是"302 直接带票据去平台"还是"跳到 MFA 页"——这是判断免验证机制的第一手证据 |
| 判读 | ① 密码是否被 JS 加密 ② `execution` 是否一次性（决定设备是否每次先 GET）③ 是否有"信任类"长效 Cookie 出现在响应里 |

### Stage 3 · 二次验证（仅在触发时执行）

若真的出现验证码页：先点 **"获取验证码"** → 抓 `#3a-sendCode`；再勾选"信任该浏览器"、填入验证码提交 → 抓 `#3b-mfa-POST`。

| 记录项 | 要求 |
| --- | --- |
| 校验接口 | URL、方法、请求体字段名（`code`？`verifyCode`？）、隐藏令牌 |
| 信任参数 | 哪个参数表示"信任"（`trustBrowser=true`？`rememberMe`？） |
| `Set-Cookie`（重要） | 逐条抄录：名称、值（脱敏）、`Expires/Max-Age`、`Path`、`Domain`、`HttpOnly`、`Secure` |
| 分类 | **会话态**（`JSESSIONID`）/ **信任态**（长效）/ **无关**（`_ga`、风控类） |
| 有效期 | 由 `Max-Age` 换算天数 → 直接决定 R8 |

### Stage 3′ · **MFA 触发条件探查矩阵（本次必做）**

因为干净浏览器没触发验证码，必须主动找出"什么条件下会触发"。**每次实验只改一个变量**，记录"是否要求验证码"。

| 编号 | 实验 | 操作要点 | 是否触发 | 结论 |
| --- | --- | --- | --- | --- |
| B1 | 清白会话重登 | 清空 Cookie、同浏览器、同网络，重登 | 已实测：**否** | 免验证不在浏览器 Cookie 上 |
| **B4** | **改 UA**（优先做） | 在 Burp `Repeater` 里把 `#2-login-POST` 的 `User-Agent` 改成 `curl/8.4.0`，重发 | TBD | 若触发 → 判定=**UA(+IP)** → **设备固定同一 UA 即可长期免验证**（最理想） |
| B2 | 换网络 | 电脑连手机热点（换 IP）后重登 | TBD | 判断是否绑定常用网络 |
| B3 | 换设备 | 另一台电脑 / 手机浏览器重登 | TBD | 判断是否绑定设备 |
| B5 | 隔日重试 | 明天同环境再登一次 | TBD | 判断是否为周期性策略（若 7 天周期，第 7 天才会触发） |
| B6 | 组合 | 异网 + 新设备 | TBD | 最可能触发，用于确认"最坏情况" |

**纪律**：此矩阵属试探性请求，**每天总登录次数 ≤ 3 次**，一旦出现滑块或异常提示立即停止。

> **结论怎么用**：任何一种条件若能用设备侧控制（UA、IP 由宿舍校园网固定），则设备可长期免验证；若触发条件不可控（如周期性 7 天），则设备必须保留 F2 验证码中继，并依赖 **窗口起点 21:00 预登录**提前把验证码需求暴露给你（21:50 已改：窗口外登录永不被平台授权，见 `docs/api-contract.md` §8.6）。

### Stage 4 · 进入平台

1. 提交后应跳回 `jzsz.uestc.edu.cn`（URL 可能带 `ticket=ST-xxxx`）→ 记为 `#4-ticket`；
2. 平台首页加载后，用 DevTools 的 `Fetch/XHR` 过滤器记录首页的一系列接口（记为 `#5-*`），特别留意：返回**用户信息**、**应用/微应用列表**、以及**微应用跳转地址**的请求。

### Stage 4′ · 点开「晚安成电」——**新窗口专项（发现 2 的应对）**

| 步骤 | 操作 | 要记录什么 |
| --- | --- | --- |
| 1 | 在**原标签页**里，先确认 DevTools 在录制；然后点击「晚安成电」图标 | 原标签页里是否有"取应用地址"的 XHR（`#5-launch-url`） |
| 2 | 新窗口/新标签页一出现，**立刻按 F12** 打开 DevTools 并勾 Preserve log / Disable cache（越早越好，否则首屏请求会丢） | — |
| 3 | 复制新窗口的**完整 URL** | 域名是否与主站不同；`?token=`/`?ticket=`/`?code=` 等一次性凭据；`appId` 等标识 |
| 4 | 在新窗口 DevTools 里刷新一次（F5），完整记录首屏请求 | `#6-app-init` 系列请求、接口基址（baseURL） |
| 5 | 回到 Burp，确认新窗口的请求**确实出现在 HTTP history**（按域名过滤） | 若没出现 → 检查代理/QUIC 设置 |

**记录模板**：

```
新窗口 URL: https://______/______?______=______
域名是否与主站相同: 是 / 否（若否，记为独立微应用）
URL 中是否含一次性凭据: 是（字段名 ______，长度 ______）/ 否
该凭据是否可重复使用: 待验（用 Repeater 重放一次即可判断）
原标签页取地址的接口: #5-launch-url = ______
新窗口的接口基址: ______
```

> ⚠️ 若发现**一次性凭据且只能用一次**，说明设备每次都必须先调 `#5-launch-url` 拿新地址——这在契约里要单独作为一个接口（EP04b），不能漏。

### Stage 5 · 点击打卡（最关键）

| 要做的事 | 在**新窗口**里等按钮变为"点击打卡"，点击它 |
| --- | --- |
| 必须抓到 | `#7-checkin-POST`（可能不止一个：有的平台先取 token 再提交）；**同时确保该窗口的 DevTools 在录制** |
| 记录重点 | URL / 方法 / `Content-Type` / 请求体**完整 JSON**（尤其 `lng`、`lat`、`address`、`timestamp`、`sign`、`token`）/ 全部自定义请求头（`X-Token`、`Authorization`、`Sign`、`Nonce`、`Timestamp`）/ 响应体 |
| 判读 | ① 是否存在**签名/加密字段**（R1 直接判据）② 坐标字段名与格式（小数位、字符串/数字）③ 坐标系（GCJ-02 / WGS-84）④ 成功响应内容 |
| 额外动作 | 打卡成功后**刷新/重进打卡页**，抓 `#8-status-after`，确认状态接口能反映"已打卡" |

> ⚠️ 打卡一天一次。**今晚就用这次真实打卡完成抓包**，不要为试错反复打卡。

### Stage 6 · 定位来源分析（纯观察）

在打卡页 DevTools → Sources 搜索 `geolocation` / `getLocation` / `qq.maps` / `amap`，或 Console 执行：

```js
navigator.geolocation.getCurrentPosition(p => console.log('RAW', p.coords));
```

| 结论 | 含义 |
| --- | --- |
| 直接调 `navigator.geolocation` | 坐标来自浏览器/系统 → **设备只能固定坐标**（与方案一致） |
| 调地图 SDK | 坐标仍来自浏览器，SDK 只做转换/展示 |
| 调后端"按 IP 定位"接口 | 设备在校园网内即可天然满足（对我们有利） |

同时记录打卡页显示的时间窗文案（**用户口述 21:00–23:30，图四文案是"每天 12:00–23:00"，请核实**）。

### Stage 7 · 判决性实验 A：信任态能否脱离原浏览器

| 实验 | 操作 | 期望结论 |
| --- | --- | --- |
| A1 同浏览器复登 | 退出登录（保留 Cookie），重新登录 | 免验证是否稳定 |
| A2 仅带信任 Cookie | 清空全部 Cookie，只写回"信任态"那几条（Burp `Repeater` 手加 `Cookie:` 头），再登录 | 仍免验证 → 纯 Cookie 机制 ✅；仍要验证码 → 绑定其他因素 ❌ |
| A3 改 UA | A2 + 更换 `User-Agent` | 是否绑定 UA（与 B4 互为印证） |
| A4 换 IP | A2 + 切换网络 | 是否绑定 IP |
| A5 换浏览器 | A2 + 第二个全新配置 | 是否存在浏览器指纹绑定 |

> 注意：既然 B1 已表明"清白浏览器也不触发"，**A2–A5 的变量组合比"Cookie"更重要**——重点记录"在什么变量下会突然要求验证码"。

### Stage 8 · 判决性实验 B：会话与票据生命周期

1. 只保留**会话态** Cookie、删掉信任态 → 重新登录 → 是否要验证码？
2. 观察 `CASTGC` 之类 SSO Cookie 是否**持久化**（有 `Max-Age`）→ 若是，设备可跳过登录页直接换票据，实现更简单；
3. 记录"关闭浏览器再打开"是否需要重新输密码。

### Stage 9 · 校园网认证抓包（UESTC-WIFI）

| 场景 | 抓什么 |
| --- | --- |
| 连接时弹出/跳转登录页（Portal） | 重定向链、portal 登录 POST（URL、参数、是否加密、是否含 `wlan_user_ip`/`nas`/`chksum`）、认证成功响应 |
| 只在 WiFi 属性里输账号密码（802.1X） | 无法用 Burp 抓（链路层）；用下面命令记录 |

```powershell
netsh wlan show interfaces
netsh wlan show profiles
netsh wlan show profile name="UESTC-WIFI" key=clear
```

需记录：**认证类型**、**加密方式**、**EAP 类型**（PEAP/TTLS）、**阶段二身份验证**、**是否校验服务器证书**、**匿名身份**、**是否要求域**。这些直接对应 ESP-IDF 的 `esp_eap_client` 配置项。

---

## 4. 关键请求记录模板（写入 `docs/capture-notes.md`）

| 字段 | 示例 |
| --- | --- |
| 编号 / 名称 | `#7-checkin-POST` |
| 阶段 | Stage 5 |
| Method + URL | `POST https://<微应用域名>/xxx/checkin` |
| 自定义请求头 | `X-Token: <脱敏>` |
| 请求体 | ```{"lng":"104.xxxxxx","lat":"30.xxxxxx","addr":"...","ts":1790000000}``` |
| 响应 | ```{"code":0,"msg":"打卡成功"}``` |
| `Set-Cookie` | `xxx=<脱敏>; Path=/; HttpOnly` |
| 是否含签名 | 否 / 是（字段：`sign`，算法未知） |
| 备注 | 需先调 `#6-status-query` 取 token；**该请求发生在新窗口** |

---

## 5. 导出与交付

1. **DevTools HAR**：Network 面板 → 右键 → `Save all as HAR with content`；**每个开过的标签页各导一份**（命名 `uestc-login-main.har`、`uestc-login-app.har`）；
2. **Burp XML**（跨标签页全覆盖，最重要）：`Proxy → HTTP history` 全选 → 右键 → `Save items` → XML → `capture/raw/burp-items.xml`；
3. **Copy as cURL**：对 `#2 / #3a / #3b / #5-launch-url / #6-* / #7 / #8` 逐个右键 → `Copy as cURL (bash)` → 汇总到 `capture/raw/key-requests.txt`；
4. **HTML/JS**：登录页 HTML、`login.js`、MFA 页 HTML（若有）；
5. **笔记**：`docs/capture-notes.md`（§4 模板 + Stage 3′/4′/6/7/8/9 结论）。

```
capture/
├─ raw/            # 原始导出（含敏感信息，切勿外传）
│  ├─ uestc-login-main.har / uestc-login-app.har
│  ├─ burp-items.xml
│  ├─ key-requests.txt
│  └─ idas_login_page.html / login.js
└─ redacted/       # 脱敏版（可交付/入库）
```

---

## 6. 脱敏要求（交付前必做）

HAR 与 Burp XML 里**明文包含密码、验证码、全部 Cookie（等于账号通行证）**。

| 敏感项 | 替换为 |
| --- | --- |
| 密码明文 / 加密串 | `$PASSWORD` |
| 验证码 | `$MFA_CODE` |
| Cookie 值（`CASTGC`、`JSESSIONID`、信任 cookie、微应用 token） | `$COOKIE_VALUE` |
| `Authorization` / `X-Token` / `sign` / 启动 token | `$TOKEN` / `$SIGN` |
| 学号、姓名、手机号 | `$STUDENT_ID` / `$NAME` / `$PHONE` |
| 宿舍精确坐标 | 建议保留数值格式，必要时打码 |

**推荐做法**：原始文件留在本机 `capture/raw/`（**不要提交仓库、不要发给我**），只把 `capture/redacted/` + 笔记交给我。

---

## 7. 自检清单（抓完对照勾选）

- [ ] 登录页 HTML 与 `login.js` 已保存，能定位到密码加密函数
- [ ] `#2-login-POST` 完整（含请求体与全部 `Set-Cookie`）
- [ ] **已记录"本次登录是否要求二次验证"**，并完成 Stage 3′ 矩阵（至少 B4）
- [ ] （若出现）二次验证的「获取验证码」与「提交验证码」两个请求都抓到
- [ ] 已抄录全部 `Set-Cookie` 并完成"会话态/信任态/无关"分类
- [ ] **已记录新窗口 URL、域名差异、是否含一次性启动凭据**
- [ ] **原标签页"取应用地址"的接口（`#5-launch-url`）已抓到**
- [ ] `#6-status-query`（判断是否已打卡的接口）已抓到
- [ ] `#7-checkin-POST` 完整（含坐标字段与是否有签名）
- [ ] `#8-status-after` 能反映"已打卡"
- [ ] Stage 6 定位来源结论已写明；**打卡页时间窗文案已核实**
- [ ] Stage 7 的 A1–A5 与 Stage 8 已完成并记录
- [ ] Stage 9 校园网认证类型与参数已记录（含 `netsh` 输出）
- [ ] 交付物已脱敏，原始文件仅存本机

---

## 8. 常见问题排查

| 现象 | 原因 | 解决 |
| --- | --- | --- |
| **新标签页的 DevTools 里没有打卡请求** | DevTools 只记录所在标签页 | 在新窗口单独按 F12；**以 Burp 为准**（发现 2） |
| **没出现二次验证** | 免验证判定不在浏览器 Cookie（很可能是 UA/IP/周期策略） | 执行 Stage 3′ 矩阵；不影响抓包，继续走 Stage 4 |
| Burp 里只有 HTTP，没有 HTTPS | 未装/未信任 CA | 重做 §2.3，务必重启浏览器 |
| 页面报"您的连接不是私密连接" | 证书装到了"个人"而非"受信任的根" | 装到 `ROOT` 存储 |
| 页面白屏/接口 400 | HTTP/2 或 TLS 协商问题 | 关闭 `Allow HTTP/2`；确认 TLS 1.2/1.3 开启 |
| 部分请求不经过 Burp | QUIC 未关 | 重做 §2.4；确认启动参数带 `--proxy-server` |
| 打卡按钮一直是"未在范围内" | 浏览器未授权定位 | 地址栏 → 位置 → 允许；开启系统定位服务 |
| 登录后一直转圈 | Intercept 开着 | 关闭 `Proxy → Intercept` |
| 提示"检测到代理/网络异常" | 站点反爬（R2） | 用普通浏览器完成关键操作；或试 Burp 隐形代理 |
| Community 关闭后历史丢失 | 不能存工程 | 关闭前先 `Save items` |
| 抓不到发送验证码的请求 | 该接口在页面加载时调用 | 在 Network 里按时间顺序翻 |

---

## 9. 收尾

1. 恢复浏览器 flags（QUIC 改回 Default）；
2. 关闭专用浏览器窗口；
3. 如不再需要，移除 `PortSwigger CA`；
4. 把 `capture/redacted/` 与 `docs/capture-notes.md` 放入项目，进入 `docs/api-contract.md` 填充与 `docs/python-test-design.md` 的 T1–T5 实验。
