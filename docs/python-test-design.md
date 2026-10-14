# PC 端 Python 参考实现与测试流程设计

> 配套文档：`docs/capture-guide.md`（抓包）、`docs/api-contract.md`（契约）。
> 本文回答一个问题：**在给 ESP32 写第一行代码之前，如何用 PC 上的 Python 把整条链路验证透彻。**
>
> **v0.2 修订（并入现场实测）**：新增 **T4b MFA 触发条件矩阵**（实测干净浏览器不触发验证码，需查清触发条件）与 **T2b 微应用进入实验**（实测点击「晚安成电」会开新窗口）；CLI 新增 `prelogin` / `launch-url` / `mfa-matrix`；`sessionstore` 明确要求**按域保存 Cookie**。

---

## 1. 定位与目的

| 维度 | 说明 |
| --- | --- |
| 是什么 | 一个跑在电脑上的 Python 工具集：解析抓包、复现登录链、完成打卡、判定关键假设 |
| 为什么需要 | ① 接口逆向的迭代速度：Python 改一行 3 秒，固件编译烧录 3 分钟起；② 设备侧没有交互式调试能力，先用 Python 把"逻辑对不对"和"接口通不通"分开验证；③ 产出的 `api-contract.md` 是固件唯一的实现依据 |
| 不是什么 | ❌ 不是最终交付形态（不会长期运行，因为家里没有常开设备）；❌ 不追求工程完美，追求**两周内得出结论**；❌ 不复制到固件里运行 |
| 后续价值 | ① 作为固件业务逻辑的"黄金参考"；② 录制响应作为 pytest fixture，用于固件回归测试；③ 接口变更时先在此验证再改固件 |

**第一原则：Python 侧只允许使用 ESP32 也能实现的算法与解析方式。**
即：AES-CBC / MD5 / SHA1 / SHA256 / Base64 / 正则 / HTML 简单解析。**任何需要执行 JavaScript 才能得到的东西，都要在报告里标红为"设备侧不可行"**——这会直接决定项目是"全自动"还是"半自动"。

---

## 2. 测试流程总览（T0–T6）

| 编号 | 实验 | 目的 | 前置 | 产出 | 预计耗时 |
| --- | --- | --- | --- | --- | --- |
| **T0** | 环境与依赖 | 跑通骨架 | Python 3.11+ | 可执行的 CLI | 30 min |
| **T1** | 契约抽取 | 从 HAR 自动生成接口契约草稿 | `capture/raw/*.har` | `api-contract.md` 骨架 + `endpoints.json` | 1–2 h |
| **T2** | **Cookie 注入式打卡** | 最快验证"打卡接口能否纯 HTTP 调用"（R1 判据） | 浏览器登录后的 Cookie | 一条成功的打卡记录 + 请求/响应样例 | 1 h |
| **T3** | 纯 HTTP 全自动 | 验证登录链（含密码加密）与 MFA 中继 | T1、T2 | 全自动脚本 + 密码加密算法确认 | 3–6 h |
| **T4** | **信任态判决实验** | 信任 Cookie 能否跨浏览器/跨 IP 用（决定能否长期无人值守） | T3 | 明确结论 + 固件必须固定的请求头清单 | 1–2 h |
| **T4b** | **MFA 触发条件矩阵**（E1 新增，必做） | 查清"什么条件下才会要求验证码" → 决定人工介入频率与固件要固定哪些头 | T3 | `api-contract.md` §6.2 填满 | 1 h + 观察数日 |
| **T2b** | **微应用进入实验**（E2 新增） | 复现"点击晚安成电"这一步：取启动地址 → 进入微应用 → 会话是否跨域可用 | T2 | `api-contract.md` EP04b 填满 | 1–2 h |
| **T5** | 定位与时间边界 | 确认坐标字段格式、坐标系、服务端校验强度、时间窗 | T2 | 固件参数（坐标常量、时间窗） | 1 h（低频） |
| **T6** | 长稳与回归 | 连续 3 天全自动成功；fixture 回归 | T3 | 进入固件 M1 的放行依据 | 3 天（挂机） |

**顺序不可颠倒**：T2 先于 T3。因为 T2 能用"浏览器现成的 Cookie"绕开登录链，30 分钟就能知道**这个项目到底能不能做**；如果 T2 就失败（例如接口有 JS 签名），T3 根本不用写。

---

## 3. 技术选型

| 用途 | 选型 | 理由 |
| --- | --- | --- |
| HTTP | `requests` + `requests.Session` | CookieJar 自动管理、重定向可控、调试直观；固件 `http_cli` 与之语义一一对应 |
| HTML 解析 | `beautifulsoup4` + `lxml` | 提取 CAS 登录页隐藏字段；固件侧对应"正则 + 字符串查找" |
| 加密 | `pycryptodome` | 复现 `login.js` 的 AES-CBC；固件侧用 mbedTLS 对应 |
| 配置 | `pydantic` + `toml`（或 `pyyaml`） | 配置校验与默认值；对应固件 `cfg_store` |
| 日志 | `rich`（可选）+ 标准 logging | 终端可读性；脱敏统一入口 |
| 重试 | `tenacity` | 退避重试策略可直接抄给固件 |
| 测试 | `pytest` | fixture 回放 |
| 行为对照（可选） | `playwright` | 用真实浏览器跑一遍并自动导出 HAR，与纯 HTTP 结果对照；排查"人点得到、脚本点不到"的差异 |
| 后续 BLE | `bleak` | M3 阶段用于模拟手机端配置设备 |

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install requests beautifulsoup4 lxml pycryptodome pydantic tenacity pytest rich
# 可选
pip install playwright bleak
```

---

## 4. 目录结构

```
tools/pc_reference/
├─ README.md                  # 本工具的使用说明（简版）
├─ requirements.txt
├─ config.example.toml        # 模板（入库）
├─ config.toml                # 真实配置（.gitignore，含凭据）
├─ cli.py                     # 统一入口（argparse 子命令）
├─ signin/
│  ├─ __init__.py
│  ├─ config.py               # 配置加载/校验/默认值
│  ├─ logkit.py               # 日志 + 统一脱敏
│  ├─ httpclient.py           # Session 封装：重定向、超时、重试、录 HAR
│  ├─ crypto.py               # AES-CBC / Base64 / 随机串（对齐 login.js）
│  ├─ cas.py                  # 登录页解析 + 提交 + 跳转判定
│  ├─ mfa.py                  # 触发验证码 / 提交验证码 / trust 参数
│  ├─ jzsz.py                 # 平台适配层：会话建立、状态查询、打卡
│  ├─ sessionstore.py         # Cookie 持久化（JSON + 权限收紧）
│  ├─ models.py               # 数据模型（CheckinResult / TrustState…）
│  ├─ contract.py             # HAR → 接口契约草稿
│  └─ replay.py               # fixture 回放（离线回归）
├─ tests/
│  ├─ fixtures/               # 录制的 HTML / JSON 响应（脱敏）
│  ├─ test_cas_parse.py
│  ├─ test_crypto.py
│  ├─ test_mfa_flow.py
│  └─ test_checkin_replay.py
└─ out/                       # 运行产物：cookie.json、日志、HAR、契约草稿
```

---

## 5. 配置设计

`config.example.toml`：

```toml
[account]
student_id = "2023xxxxxx"
password   = "REPLACE_ME"          # 或改用环境变量 SIGNIN_PASSWORD
mfa_type   = "sms"                 # sms | wecom

[net]
timeout_s     = 15
retry         = 3
verify_tls    = true
user_agent    = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) ... Chrome/xxx"  # 与抓包完全一致
proxy         = ""                 # 需要时填 http://127.0.0.1:8080

[urls]
cas_login   = "https://idas.uestc.edu.cn/authserver/login"
platform    = "https://jzsz.uestc.edu.cn/"
# 以下三项由 T1 填充
status_api  = "TBD"
checkin_api = "TBD"
sendcode_api = "TBD"

[location]
lng = 104.000000                   # 由 T5 填真实抓包值
lat = 30.000000
address = "电子科技大学xx宿舍"
coord_sys = "gcj02"

[schedule]
target = "22:00"
window_start = "21:00"
window_end = "23:30"
```

**凭据管理**：密码优先从环境变量 `SIGNIN_PASSWORD` 读取；`config.toml`、`out/cookie.json` 均写入 `.gitignore`；`sessionstore` 落盘后把文件权限收紧为仅当前用户可读。

---

## 6. 模块设计

### 6.1 `logkit.py` — 日志与脱敏

| 函数 | 说明 |
| --- | --- |
| `setup(level)` | 初始化 logging，格式含时间/级别/模块 |
| `mask_secret(v, keep=6)` | `abcdef123456` → `abcdef…` |
| `mask_cookie(header)` | 只保留 cookie 名与首 6 位 |
| `mask_body(text, keys)` | 对 `password/code/token/sign` 字段做替换 |
| `dump_http(req, resp)` | 统一打印请求/响应（**唯一允许打印 HTTP 的地方**，保证不会漏脱敏） |

### 6.2 `httpclient.py` — 传输层

```python
class HttpClient:
    def __init__(self, cfg, har_path: Path | None = None): ...
    def request(self, method: str, url: str, *, data=None, json=None,
                headers=None, allow_redirects=False, timeout=None) -> Response: ...
    def follow(self, url: str, max_hops: int = 8) -> Response:
        """手动跟随 302，逐跳打印 Location（对应固件 http_cli 的行为）"""
    @property
    def cookies(self) -> dict[str, str]: ...
    def load_cookies(self, path: Path) -> None: ...
    def save_cookies(self, path: Path) -> None: ...
```

关键点：
- **默认不自动重定向**（`allow_redirects=False`），手动跟随并记录每一跳，这样才能看清 CAS 票据流向，也保证与固件行为一致；
- 每次请求写入 HAR（`out/trace.har`），便于事后对照；
- 超时、重试（`tenacity`：3 次，1/2/4 s 退避，仅对网络异常重试，**不对 4xx/5xx 重试**——避免风控）。

### 6.3 `crypto.py` — 密码加密（对齐 `login.js`）

需要支持的典型实现（**以抓到的 `login.js` 为准，下面两种最常见**）：

| 变体 | 算法 |
| --- | --- |
| 金智/CAS 常见 | `key = pwdDefaultEncryptSalt(16 字节)`；`iv = 16 字节随机串`；`out = base64(iv + AES-128-CBC(key, iv, plain))`，`plain` 可能是 `salt + password` 或 `password` |
| 简单变体 | AES-128-ECB(key=固定串) 后 base64 |

```python
def encrypt_password(plain: str, salt: str) -> str:
    """必须与 login.js 的 encryptPassword 完全一致；T3 会用真实响应验证。"""
def random_string(n: int, alphabet: str) -> str: ...
```

**验证方式（T3 的关键动作）**：在登录页把浏览器 `login.js` 的加密函数在 Console 里跑一次，得到密文；再用 `crypto.py` 对同一明文/同一 salt 加密，**比对是否一致**（同一 IV 才可比；若 IV 随机则比对"解密回来是否等于明文"）。

### 6.4 `cas.py` — 登录

| 步骤 | 方法 | 说明 |
| --- | --- | --- |
| 取登录页 | `fetch_login_page()` → `LoginPage` | 返回 `execution` / `pwdDefaultEncryptSalt` / 表单 action / 隐藏字段 dict |
| 解析 | `parse_hidden_fields(html)` | BeautifulSoup 取 `input[type=hidden]`；固件侧对应正则 |
| 提交 | `submit(username, password, page)` | `allow_redirects=False`，按 `Location` 判定：平台票据 / MFA 页 / 错误 |
| 判定 | `classify(resp)` → `TRUSTED \| MFA_REQUIRED \| BAD_CREDENTIAL \| CAPTCHA \| UNKNOWN` | 用明确的枚举，方便与固件状态机对齐 |

> 必须处理"一次性令牌"：若 `execution` 每次提交后失效，则固件也必须"先 GET 再 POST"，不能复用。T3 要专门验证这一点。

### 6.5 `mfa.py` — 二次验证中继

```python
def request_code(client, phone_hint: str) -> None:      # 触发发送验证码
def submit_code(client, code: str, trust: bool = True) -> Response:
def detect_trust_cookie(before: set[str], after: set[str]) -> list[Cookie]: ...
```

- `code` 由**人工输入**（`input()` 或 CLI 参数）——完全对应设备侧"BLE 回传验证码"的交互；
- 记录 `trust=True` 时服务端新增的长效 Cookie，写入 `out/trust.json`（这是最终要烧进设备的东西）；
- 打印"信任有效期剩余天数"。

### 6.6 `jzsz.py` — 平台适配层（**唯一随接口变化而改的模块**）

```python
def ensure_session(client) -> SessionInfo:      # 票据换会话、必要的初始化接口
def get_launch_url(client, app_id: str) -> str: # E2：取微应用启动地址（可能含一次性 token）
def query_status(client, day: str) -> StatusResult:   # 未打卡 / 已打卡
def checkin(client, lng: float, lat: float, address: str) -> CheckinResult: ...
def verify_checkin(client, day: str) -> bool:   # 打卡后复查询
```

每个函数内部按 `api-contract.md` 组装请求；**不允许在此模块之外出现 URL 与字段名**。

> **E2 相关注意**：`get_launch_url` 的返回值可能带一次性凭据。Python 侧要顺手验证"**同一凭据能否重复使用**"（用 `requests` 连续请求两次即可判定），并把结论写进契约——这决定固件是缓存启动地址还是每次重取。
>
> **多域 Cookie**：`sessionstore` 必须按域保存 Cookie；若微应用是独立域名，`requests.Session` 会自动按域发送，但导出的 JSON 要保留 `domain` 字段，否则固件侧无法还原。

### 6.7 `sessionstore.py` / `models.py`

- `sessionstore`：`save/load/merge` Cookie JSON，记录每个 Cookie 的 `domain/path/expires/httpOnly`，支持"只导出长效 Cookie"；
- `models`：`CheckinResult(ok, msg, raw, ts)`、`TrustState(cookies, expire_at)`、`StatusResult(checked_in, raw)`。

### 6.8 `contract.py` — HAR → 契约草稿

```python
def extract(har_path: Path, hints: dict[str, str]) -> list[Endpoint]: ...
def render_markdown(endpoints, out_path: Path) -> None: ...
```

对每个命中的 URL 输出：方法、路径、query、请求头（去掉通用头）、请求体字段与示例值、响应状态与字段、`Set-Cookie` 列表。**自动生成的草稿仍需人工确认**（尤其"哪个是打卡接口"）。

---

## 7. CLI 设计

```powershell
# 0) 环境自检
py cli.py doctor

# 1) 契约抽取（T1）
py cli.py har2contract --har ..\..\capture\raw\uestc-login.har --out out\endpoints.json

# 2) Cookie 注入式打卡（T2）：先用浏览器登录，导出 cookie.json
py cli.py cookies import --from-browser-file out\browser-cookies.json
py cli.py status                       # 期望：未打卡/已打卡
py cli.py checkin --dry-run            # 只组装请求并打印（不真正提交）
py cli.py checkin                      # 正式打卡
py cli.py verify                       # 打卡后确认

# 2b) 微应用进入（T2b，E2）
py cli.py launch-url                   # 取微应用启动地址，打印 URL、域名、是否含一次性凭据
py cli.py launch-url --repeat          # 用同一地址连续请求两次，判定凭据是否一次性

# 3) 纯 HTTP 全自动（T3）
py cli.py login                        # 需要验证码时：等待输入 code
py cli.py login --code 123456          # 或直接传参（便于脚本化）
py cli.py prelogin                     # 模拟固件窗口起点 21:00 预登录（每天只跑一次登录链；窗口外登录永不被平台授权）
py cli.py auto                         # 登录 + （必要时）打卡 + 确认，一条龙

# 4) 判决实验（T4 / T4b）
py cli.py trust-export --out out\trust.json     # 只导出长效信任 Cookie
py cli.py trust-test --trust out\trust.json --ua "curl/8.4.0"   # 变更 UA 重试
py cli.py trust-test --trust out\trust.json --ip-hint hotspot    # 提示人工切换网络后回车
py cli.py mfa-matrix --case B4 --ua "curl/8.4.0"   # MFA 触发条件矩阵：逐项执行，记录是否要求验证码
py cli.py mfa-matrix --report                       # 汇总输出 api-contract §6.2 表格

# 5) 定位/时间边界（T5）
py cli.py probe-location --lng 104.000000 --lat 30.000000 --offset-m 100
py cli.py probe-window  # 读取页面/接口返回的时间窗与状态

# 6) 回归
pytest -q
```

统一约定：退出码非 0 表示失败；每个命令输出最后一行是**机器可读的 JSON 摘要**（便于事后统计），例如：

```json
{"cmd":"checkin","ok":true,"ts":"2026-10-11T22:00:07+08:00","resp_code":0,"msg":"打卡成功"}
```

---

## 8. 错误分类与退出码

| 退出码 | 类别 | 含义 | 固件侧对应 |
| --- | --- | --- | --- |
| 0 | OK | 成功 | `EVT_RESULT(ok)` |
| 10 | 网络 | 连接/超时/DNS/TLS | `ERR_NET` → 退避重试 |
| 20 | 认证 | 账号密码错误 | `ERR_BAD_CRED` → 告警（不重试） |
| 21 | 认证 | 需要验证码 | `EVT_NEED_CODE` |
| 22 | 认证 | 验证码错误/过期 | `ERR_CODE_INVALID` → 允许重试 |
| 30 | 接口 | 页面结构变更/字段缺失 | `ERR_API_CHANGED` → 停止盲试 + 告警 |
| 31 | 接口 | 触发风控/验证码图片/滑块 | `ERR_RISK` → 人工介入 |
| 40 | 业务 | 时间段不允许 | `ERR_WINDOW` → 等到时间窗 |
| 41 | 业务 | 坐标不在范围内 | `ERR_GEO` → 检查坐标常量 |
| 50 | 业务 | 今日已打卡 | 视为成功（幂等） |
| 60 | 内部 | 配置错误 | `ERR_CONFIG` |

**这张表就是固件错误码的来源**，两边保持一致能省掉大量联调时间。

---

## 9. 与 ESP32 固件的映射

| Python | 固件模块 | 备注 |
| --- | --- | --- |
| `httpclient.py` | `http_cli` + `esp_http_client` | 手动重定向、CookieJar、超时与重试策略一一对应 |
| `crypto.py` | `cas_auth` 内的 mbedTLS | AES-128-CBC、Base64 |
| `cas.py` | `cas_auth` | 登录页正则解析（固件用 `esp_http_client` + 自写正则） |
| `mfa.py` | `cas_auth` + `ble_svc` | Python 的 `input()` ↔ 设备的 BLE `code` 指令 |
| `jzsz.py` | `jzsz_api` | 契约字段直接照搬 |
| `sessionstore.py` | `cfg_store`（NVS） | Cookie 持久化与掉电恢复 |
| `config.py` | `cfg_store` | 配置项字段名保持一致，便于导出 |
| `logkit.py` | `log_ring` + `notify` | 脱敏规则照搬 |
| CLI 退出码表 | 固件错误码枚举 | 见 §8 |

**从 Python 到设备的配置导出**（M3 阶段直接复用）：

```powershell
py cli.py export-ble-config --out out\ble-config.json
# 生成：
# {"op":"wifi.set","ssid":"UESTC-WIFI","pass":"..."}
# {"op":"auth.set","user":"2023xxxxxx","pass":"..."}
# {"op":"loc.set","lng":104.xxxxxx,"lat":30.xxxxxx,"addr":"..."}
# {"op":"trust.import","seq":0,"of":1,"data":"<base64>"}
```

---

## 10. 测试用例（pytest）

| 用例 | 断言 |
| --- | --- |
| `test_cas_parse` | 从 fixture HTML 中提取出 `execution` 与 `pwdDefaultEncryptSalt`（非空） |
| `test_crypto` | 用固定 salt/iv 的加密结果与 `login.js` 的已知输出逐字节一致 |
| `test_redirect_classify` | 各样例响应被正确分类为 `TRUSTED / MFA_REQUIRED / BAD_CREDENTIAL` |
| `test_trust_cookie_detect` | 从两组 Set-Cookie 中正确识别出长效信任 Cookie |
| `test_status_replay` | 用录制的状态响应正确解析 `checked_in` |
| `test_checkin_replay` | 用录制的成功响应正确解析 `ok=True`；用失败响应解析出错误码 |
| `test_mask` | 脱敏函数不泄漏密码/cookie/验证码 |

---

## 11. 运行手册（按序执行）

| 步骤 | 操作 | 判定标准 |
| --- | --- | --- |
| 1 | 完成 `docs/capture-guide.md` 抓包 | 自检清单全勾选 |
| 2 | `cli.py doctor` + `har2contract` | 生成契约草稿，人工确认打卡接口 |
| 3 | 浏览器登录一次，导出 Cookie → `cli.py status` / `checkin` | **T2 通过 = 项目可行**（R1 解除） |
| 3b | `cli.py launch-url [--repeat]`（E2） | 得到 EP04b 结论：启动地址从哪来、凭据是否一次性 |
| 4 | `cli.py login`（人工输验证码） | T3 通过 = 登录链可复现 |
| 5 | `cli.py trust-test` 系列 | 得到 R3 结论 + 固件必须固定的请求头清单 |
| 5b | `cli.py mfa-matrix`（B1/B4/B2/B3，必要时隔日 B5） | 得到 T4b 结论 = **人工介入频率** |
| 6 | `cli.py probe-location / probe-window`（低频，1–2 次） | 得到坐标常量与时间窗参数 |
| 7 | 挂机 3 天（`auto` + Windows 计划任务，可临时替代设备） | T6 通过 = 放行固件 M1 |

> 第 7 步顺带解决一个问题：**在设备做出来之前，你已经有 3 天不需要手动打卡了**。

**风控纪律（全程遵守）**：所有"试探性"请求（改坐标、改 UA、改 IP）都手动、低频、每天 ≤ 3 次；正式打卡每天仅一次；一旦出现滑块或异常提示，立即停止当天所有自动化。

---

## 12. 安全与数据管理

| 项 | 要求 |
| --- | --- |
| `config.toml` / `out/cookie.json` / `out/trust.json` | 已加入 `.gitignore`；文件权限仅当前用户 |
| 日志 | 只经 `logkit` 输出；默认 `INFO`，`--debug` 也不打印明文密码/cookie |
| HAR | `out/trace.har` 含敏感信息，**不入库** |
| 交付 | 只提交 `docs/*` 与脱敏 fixture；`capture/raw/`、`config.toml`、`out/` 一律不提交 |
| 用完即清 | T6 结束后删除 `out/cookie.json`、`out/trust.json`（设备上线后由其保管） |
