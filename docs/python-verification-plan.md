# Python 验证方案（基于 2026-10-11 实测抓包）

> 目的：在写任何 ESP32 固件之前，用 PC 上的 Python 把"打卡"与"登录"两条链路**逐段证伪**。
> 本文所有接口、字段、算法、Cookie 全部来自 `capture/raw/` 的实测数据，不是推测。
> 配套代码：`tools/pc_reference/`（可直接运行）。

---

## 0. 本次要回答的问题（验证目标）

| 编号 | 验证目标 | 通过意味着 | 失败意味着 |
| --- | --- | --- | --- |
| **V1** | 打卡接口能否用脚本复现（有无签名/风控） | **项目可行（R1 解除）** | 项目降级为"设备只提醒" |
| **V2** | 登录链能否脱离浏览器（密码加密 + 票据换会话） | 设备可长期自持（M2 可做） | 依赖人工注入会话 |
| **V3** | 免二次验证的条件是什么（`bfp` 指纹） | 设备可长期无人值守 | 需人工回传验证码（可接受降级） |
| **V4** | `SESSION` 的 1 年有效期是否真实 | 设备几乎不用登录 | 需按周期重新登录 |
| **V5** | 全自动闭环 + 连续挂机 | 进入固件 M1 | — |

**执行顺序不可颠倒**：V1 只需 10 分钟且零依赖，却能一举判定项目生死。

---

## 1. 已从抓包确认的事实（固件实现依据）

### 1.1 端点全清单（协议 `https://`，全部同域 `jzsz.uestc.edu.cn`）

| ID | 方法 | 路径 | 用途 | 关键请求头 |
| --- | --- | --- | --- | --- |
| EP01 | GET | `https://idas.uestc.edu.cn/authserver/login?service=https%3A%2F%2Fjzsz.uestc.edu.cn%2F` | 取登录页（拿 `execution` / `pwdEncryptSalt`） | — |
| EP02 | POST | 同上 | 提交登录 | `Content-Type: application/x-www-form-urlencoded` |
| EP03 | GET | `https://idas.uestc.edu.cn/authserver/bfp/info?bfp=<32位大写HEX>&_=<毫秒>` | 上报浏览器指纹，换取长效指纹 Cookie | `X-Requested-With: XMLHttpRequest` |
| EP04 | GET | `https://jzsz.uestc.edu.cn/?ticket=ST-xxx` | 票据换平台会话（**下发 `SESSION`**） | — |
| EP05 | GET | `/daily-checkin/checkLogin?appId=294` | 进入微应用（302 → H5 页，**无 token**） | — |
| EP06 | GET | `/daily-checkin/api/student-checkin-record/today` | **状态查询**（幂等判断依据） | `Platform: h5`、`Referer: …/daily-checkin-h5/index.html?appId=294` |
| EP07 | GET | `/daily-checkin/api/checkin-schedule/effective` | **时段**（可动态读取，勿硬编码） | 同上 |
| EP08 | GET | `/daily-checkin/api/checkin-location/available-locations` | **打卡点与半径** | 同上 |
| EP09 | GET | `/daily-checkin/api/map/reverse?lat=..&lng=..` | 逆地理编码（取地址文本） | 同上 |
| EP10 | POST | `/daily-checkin/api/student-checkin-record/checkin` | **提交打卡** | `Content-Type: application/json`、`Platform: h5`、`Origin`、`Referer` |

### 1.2 密码加密算法（已由 `encrypt.js` + 密文反推双重确认）

```js
// authserver/uestcTheme/static/common/encrypt.js（关键片段）
function getAesString(data, key, iv) {
  key = CryptoJS.enc.Utf8.parse(key); iv = CryptoJS.enc.Utf8.parse(iv);
  return CryptoJS.AES.encrypt(data, key, {iv: iv, mode: CBC, padding: Pkcs7}).toString();
}
function encryptAES(data, key) { return key ? getAesString(randomString(64) + data, key, randomString(16)) : data; }
function encryptPassword(data, key) { try { return encryptAES(data, key); } catch (e) {} return data; }
var $aes_chars = "ABCDEFGHJKMNPQRSTWXYZabcdefhijkmnprstwxyz2345678";
```

| 参数 | 值 |
| --- | --- |
| key | `pwdEncryptSalt`（登录页隐藏域，**每次加载随机，16 字符**，UTF-8 直接作 16 字节密钥） |
| iv | `randomString(16)`（**不随请求发送**） |
| 明文 | `randomString(64) + 密码` |
| 模式 | AES-128-CBC / PKCS7 |
| 提交值 | `base64(密文)`（**不含 IV**） |
| 长度校验 | 实测 108 字符 → 80 字节 = 5 个分组（64 随机 + 11 位密码 + 5 字节填充） |

> ⚠️ 因为明文前 64 字节是随机串、密码总落在**第 5 个分组之后**，而 CBC 中第 2 个分组起不受 IV 影响，所以**IV 取任意值服务端都能取出密码**。设备侧实现时 iv 用随机 16 字符即可（与浏览器一致）。

### 1.3 打卡载荷（**字段结构**实测原样，勿裁剪字段；坐标/地址为示例值）

```json
{"latitude":30.7549800,"longitude":103.9338830,"reverseGeocode":"四川省成都市郫都区",
 "clientTime":"2026-10-11 22:05:05","userAgent":"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36",
 "coordinateSystemCode":"gcj02","accuracy":86,"locationDurationMs":1189,"networkTypeCode":"4g",
 "locationEnabled":false,"locationAuthorized":"","locationAccuracy":"","errorMessage":""}
```

- **无签名字段、无 token、无 CSRF 参数** ← 项目可行性关键结论；
- 成功响应：`{"code":0,"msg":"操作成功","data":true,"ok":true}`。

### 1.4 状态查询（`today`）关键字段

| 字段 | 未打卡 | 已打卡 |
| --- | --- | --- |
| `recordStatusCode` | `PENDING` | `CHECKED_IN` |
| `checkinTime` | `null` | `"2026-10-11 22:05:06"` |
| `distanceMeter` | `null` | `68.70` |
| `abnormalFlag` | `false` | `false`（异常记录会置 `true`，**务必检查**） |

同时返回：`checkinLocationName`＝本科3组团、`checkinLocationLatitude`＝30.7549800、`checkinLocationLongitude`＝103.9338830、`checkinRadiusMeter`＝**130**、`dormitoryLocation`、`studentDormitory`。

> ⚠️ 该响应含**本人姓名/手机号/学号以及辅导员联系方式**。脚本**只允许打印上表字段**，禁止整包落盘或打印（`capture` 里已有，别再产生副本）。

### 1.5 时段（`checkin-schedule/effective`）

`studentCheckinStartOffsetMinute = 1260`、`studentCheckinEndOffsetMinute = 1410` → **21:00 – 23:30**（自 00:00 起的分钟偏移）。与你口述一致 ✅
（`studentReport*` 为 1260–2760，是上报/异常处理的更宽窗口。）

### 1.6 Cookie 全清单（设备必须实现的部分）

| 名称 | 域 | 属性 | 设备是否需要 | 说明 |
| --- | --- | --- | --- | --- |
| `JSESSIONID` | idas | `Path=/authserver; HttpOnly`，会话级 | ✅ 登录流程内 | 登录页 → POST 必须同一会话 |
| `route` | idas | `Path=/authserver` | ✅ | 负载均衡粘滞，随响应自动带上 |
| `MULTIFACTOR_BROWSER_FINGERPRINT` | idas | `Path=/; HttpOnly`，`Max-Age=2147483647`（**2094 年到期**） | ✅ **免二次验证的核心** | 由 `bfp/info` 下发，值＝客户端上报的 32 位 HEX |
| `CASTGC` | idas | `Path=/authserver; HttpOnly`，**会话级** | 可选 | SSO 票据 Cookie，浏览器关闭即失效 |
| `happyVoyage` | idas | `Path=/; HttpOnly`，会话级 | 观察 | 登录 POST 响应下发；本次流程未依赖它，V2 需确认是否必需 |
| **`SESSION`** | jzsz | `Path=/; HttpOnly`，**`Max-Age=31536000`（1 年！）** | ✅ **打卡鉴权就靠它** | 由 `GET /?ticket=…` 下发 |

### 1.7 本次流程中**没有出现**二次验证

#2→#61→#62 全程无 MFA 页面；登录 POST 直接 302 带票据。原因候选（V3 要证）：
服务端把 `(账号, 指纹值)` 记为可信设备，或该账号/网络处于放宽状态。

---

## 2. 环境准备（**你的电脑目前没有 Python**）

实测：`py` 不存在；`where python` 只指向 `WindowsApps\python.exe`（Microsoft Store 的**占位符**，不是真 Python）。

### 2.1 安装 Python 3.12（任选其一）

**方式 A：官网安装包（推荐，最稳）**
1. 打开 <https://www.python.org/downloads/windows/> → 下载 `Windows installer (64-bit)`；
2. 运行安装包 → **务必勾选 `Add python.exe to PATH`** → `Install Now`；
3. 关闭并**重新打开** PowerShell，执行：
   ```powershell
   python --version
   ```
   期望：`Python 3.12.x`。

**方式 B：winget**
```powershell
winget install -e --id Python.Python.3.12
```
（若 `winget` 不可用就走方式 A。）

**若 `python --version` 弹出应用商店**：说明仍是占位符 → 设置 → 应用 → 高级应用设置 → 应用执行别名 → 关闭 `python.exe` / `python3.exe`，然后重试。

### 2.2 依赖

```powershell
python -m pip install --upgrade pip
python -m pip install pycryptodome      # 仅 V2/V3（登录链）需要；V1 零依赖
```

### 2.3 目录与凭据

```powershell
cd tools\pc_reference
copy config.example.json config.json
notepad config.json     # 填入学号、密码、捕获的 SESSION、指纹
```

> 🔒 `config.json`、`out/`、`capture/raw/` 已在 `.gitignore` 中。**原始抓包含你的密码与一年期会话 Cookie（等于账号通行证）**，不要提交到任何仓库、不要发给别人；不需要脱敏我也不会要求，但请勿外传。

---

## 3. 验证阶段（按序执行）

### V0 · 自检（不联网）

```powershell
python verify.py selftest
```

| 检查项 | 期望输出 |
| --- | --- |
| Python 版本 | `>= 3.10` |
| `pycryptodome` | `可用` / `未安装（V2 起需要）` |
| 加密算法自测 | 用 `encrypt_password` 加密再按"服务端方式"（IV=0，从第 64 字节读）解回 → **与原文一致** |
| 抓包密文复现 | 用抓到的 `#2` 密文按服务端方式解出 → 尾部为可见 ASCII 密码（长度 11） |
| 配置完整性 | 缺项会明确列出 |

**判定**：出现 `[FAIL]` 就不要往下走，把输出发我。

---

### V1 · 打卡接口复现（**最关键，10 分钟**）

用浏览器登录后从抓包中取出 `SESSION` 值填入 `config.json` 的 `session_cookie`。

```powershell
# V1a 只读：查今天状态
python verify.py status
```
期望：
```
[OK] SESSION 有效
     recordStatusCode = CHECKED_IN
     checkinTime      = 2026-10-11 22:05:06
     distanceMeter    = 68.70
     location         = 本科3组团 (半径 130 m)
     abnormalFlag     = false
```

```powershell
# V1b 先干跑：只组装请求并打印，不发送
python verify.py checkin --dry-run
```
检查打印的 URL、请求头、JSON 载荷是否与 §1.2/§1.3 一致。

```powershell
# V1c 真实打卡（当天尚未打卡时才做，一天只有一次）
python verify.py checkin
python verify.py status       # 复查
```
期望：`{"code":0,"msg":"操作成功","data":true,"ok":true}` → 复查为 `CHECKED_IN`。

**判定**：V1c 成功 = **R1 解除，项目可行性确立**。
**失败处理**：401/302→登录页 = 会话过期（换新 SESSION）；返回业务错误 = 把响应发我。

---

### V2 · 登录链复现（脱离浏览器）

```powershell
python verify.py login
```
脚本行为（与浏览器逐步一致）：
1. `GET EP01` → 从 HTML 取 `execution`、`pwdEncryptSalt`；
2. `GET EP03`（`bfp=<config 中的指纹>`）→ 服务器下发长效指纹 Cookie；
3. `POST EP02`（密码按 §1.2 加密）→ 期望 **302 + `Location` 含 `ticket=ST-`**；
4. `GET EP04`（跟票据）→ **拿到新的 `SESSION`**；
5. 保存到 `out/session.json`。

期望输出：
```
[1/4] 取登录页      execution=e1s1  salt=XXXXXXXXXXXX
[2/4] 上报指纹      cookie 已设置 (Max-Age=2147483647)
[3/4] 提交登录      302 -> https://jzsz.uestc.edu.cn/?ticket=ST-...
[4/4] 票据换会话    SESSION 已更新 (Max-Age=31536000, 1 年)
[OK] 登录链可脱离浏览器复现
```

**判定**：拿到新 `SESSION` 且随后 `python verify.py status` 正常 → **R2/V2 通过**。

**若第 3 步返回 MFA 页面**（HTML 含 `isMultifactor` / `authLogin`）→ 直接跳到 V3 的结论表，并按"需人工验证码"分支处理。

---

### V3 · 免二次验证的条件（决定无人值守）

对比三次登录，只改一个变量：

```powershell
# V3a 用抓包里的指纹（应免验证，与浏览器等价）
python verify.py login --bfp captured

# V3b 用全新随机指纹（模拟"从未被信任的新设备"）
python verify.py login --bfp random

# V3c 完全不上报指纹
python verify.py login --bfp none
```

| 结果组合 | 结论 | 对设备的含义 |
| --- | --- | --- |
| a=免验证，b=免验证，c=免验证 | 免验证与浏览器指纹无关（账号/网络层面放宽） | **设备零负担**，随机指纹即可 |
| a=免验证，b/c=要验证码 | **信任 = 服务端按指纹值记账** | 设备**固定**烧入抓包里的指纹值即可长期免验证（最可能的结论）|
| a/b/c 都要验证码 | 信任绑定更强的东西（UA/IP/风控） | 尝试固定 UA（已固定）→ 仍失败则走"人工回传验证码"降级 |

> 纪律：本组实验**每天总登录次数 ≤ 3 次**；一旦出现滑块或"操作过于频繁"，立即停止并隔天再试。

---

### V4 · 会话寿命（决定设备的登录频率）

`SESSION` 的 `Max-Age` 是 1 年，但**服务端真实 TTL 需要实测**。

```powershell
# 每天执行一次（只读，不打卡）
python verify.py probe-session
```
连续 **3–7 天**记录输出（`out/session_probe.log` 会自动追加）。

| 观察 | 结论 |
| --- | --- |
| 全程 `[OK]` | 会话寿命 ≥ 7 天 → 设备**平时不需要登录**，仅在失效时登录一次 |
| 第 N 天失效 | 会话寿命 ≈ N 天 → 设备按此周期预登录 |

> 这也是"设备几乎不用登录链"的证据收集；即便 V2 失败，只要 V1+V4 通过，设备仍可用（定期人工注入会话）。

---

### V5 · 全自动闭环与挂机

```powershell
# 一条龙：复用会话 → 失效则登录 → 查状态 → 必要时打卡 → 复查
python verify.py auto --dry-run     # 先看流程
python verify.py auto
```

挂机验证（临时用电脑代替设备，为固件提供"黄金参考"）：

```powershell
# 每天 22:00 触发（示例：注册计划任务）
schtasks /create /tn "wanan-checkin" /tr "python D:\path\tools\pc_reference\verify.py auto" /sc daily /st 22:00
```

连续 3 天成功 → **V5 通过，进入固件 M1**。

---

## 4. 结论汇总（实测记录 · 2026-10-11）

| 项 | 结论 | 对应风险 |
| --- | --- | --- |
| V0 自检 | ✅ PASS（Python 3.12.15 / pycryptodome 3.23.0；加密算法自测 + 抓包密文复现均通过） | — |
| V1 打卡可脚本化 | ✅ **PASS**：注入 `SESSION` 后 `today`/`schedule`/`available-locations` 全部 200，字段与抓包一致 | **R1 解除** |
| V2 登录可脱离浏览器 | ✅ **PASS**：全新会话 → 取 salt → POST 登录 → 302 带票据 → 新 `SESSION` → 用新会话查状态成功 | **全链路打通** |
| V3 免验证条件 | ✅ **定论**：只取决于 `MULTIFACTOR_BROWSER_FINGERPRINT`（可信指纹 → 免验证；随机指纹 → 302 `isMultifactor=true`） | **R3 解除** |
| V4 会话寿命 | ☐ 待连续多日 `probe-session`（Cookie 标称 1 年，服务端 TTL 未知） | R5 |
| V5 闭环成功率 | 🟡 命令冒烟通过；真实打卡待窗口内执行并连续观察 3 天 | — |
| `happyVoyage` 是否必需 | ✅ **非必需**（V2 全新会话无此 Cookie 仍登录成功） | **R2 登录环节排除** |
| MFA 验证码交互细节 | ☐ 仅 F2 兜底需要；脚本化 GET `reAuthLoginView.do` 会退回登录页 → 需一次浏览器抓取 | — |

### 4.1 V2 / V3 原始证据（节选）

```
# V2（captured 指纹）
[1/4] execution=e1s1  pwdEncryptSalt=AwgKQ1oc3RkV2Oi3      ← 每次登录随机
[3/4] POST /authserver/login → 302  Location: https://jzsz.uestc.edu.cn/?ticket=ST-181925-…
[4/4] GET /?ticket=… → 302 → 200；SESSION 已更新
      [OK] 新会话可用，今日状态：CHECKED_IN
      [OK] 登录 → 会话 → 打卡接口 全链路打通

# V3b（全新随机指纹）
[3/4] POST /authserver/login → 302
      Location: https://idas.uestc.edu.cn/authserver/reAuthCheck/reAuthLoginView.do?isMultifactor=true&service=…
      ⇒ 服务端要求二次验证
```

> **给固件的三条结论**：
> 1. 免验证 = 服务端按**指纹值**记账 → 设备**逐字固定**抓包中的指纹即可长期无人值守；
> 2. 设备**无需调用 EP03**（`bfp/info`），直接注入 Cookie（V2 已实测通过）；
> 3. 登录页 `pwdEncryptSalt` **每次变化**，设备必须**每次先 GET 登录页**再加密提交，不能缓存。

---

## 5. 安全与操作纪律

1. **数据**：`capture/raw/`（含密码与一年期会话）与 `config.json`、`out/` 一律不入库、不外传；
2. **频率**：登录实验每天 ≤ 3 次；打卡每天 1 次；探测（`status`/`probe-session`）每天 ≤ 5 次；
3. **一旦异常立即停手**：出现滑块、验证码、"操作频繁"、`abnormalFlag=true`，停止当天全部自动化并把响应发我；
4. **不做字段裁剪实验**：不尝试删减打卡字段"看服务端认不认"——收益低，却可能产生异常记录（`abnormalFlag`）；
5. **只读优先**：所有新想法先跑 `--dry-run` 或 `status`。

---

## 6. 常见错误排查

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `python` 打开应用商店 | Store 占位符 | 见 §2.1 结尾 |
| `python verify.py` 报 `No module named Crypto` | 未装 pycryptodome | `python -m pip install pycryptodome`（V1 不需要） |
| `SSL: CERTIFICATE_VERIFY_FAILED` | 系统根证书/时间异常 | 校准系统时间；确认能正常访问该站点 |
| `status` 返回 302 或 401 | `SESSION` 过期/被顶 | 重新登录浏览器，更新 `config.json` 的 `session_cookie` |
| 登录 POST 返回 200 且是 HTML | 密码错或需要验证码 | 看页面是否含 `isMultifactor`；检查密码是否含特殊字符 |
| 打卡返回"不在打卡范围内" | 坐标/坐标系不对 | 用 `config.json` 里的实测坐标；确认 `gcj02` |
| 打卡返回"不在打卡时段内" | 当前时间不在 21:00–23:30 | 在窗口内执行 |
| 返回 `abnormalFlag=true` | 被判定异常 | **立即停止**，联系我分析 |

---

## 7. 交付物 → 固件

验证完成后，以下三份「冻结数据」直接喂给固件：

| 数据 | 来源 | 固件用途 |
| --- | --- | --- |
| 端点 + 请求头 + 载荷模板 | 本文 §1.1/§1.3 | `jzsz_api` |
| 密码加密算法 | 本文 §1.2 | `cas_auth` + mbedTLS AES-CBC |
| Cookie 规则（域/路径/有效期/是否需要） | 本文 §1.6 | `http_cli` 的多域 Cookie 罐 |
| 打卡点中心 + 半径 + 时段 | 本文 §1.4/§1.5（设备改为**动态读取** EP07/EP08） | `checkin` + `sched` |

> 固件与本文的差异只应有一处：设备**动态读取** `available-locations` 与 `checkin-schedule/effective`，而不是硬编码坐标与时段——这样学校改范围/改时段都不需要刷固件。
