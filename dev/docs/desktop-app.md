# 桌面 1.1.0 架构更新

2026-10-27：微信票据链已移植，桌面版本独立为 1.1.0。`wechat_wire.py` 负责字节编解码、PyCryptodome GCM/ECDH；`wechat_transport.py` 负责有界 socket/HTTPDNS/MMTLS/0-RTT；`wechat.py` 负责 OAuth 签名、轮换落盘、两 code 和独立 MP Cookie 取票。业务 `service._checkin_one` 在 submit 前取得账号自己的票据，`CheckinApi.submit` 拒绝缺失或非法票据。

每账号配置新增 wechat 对象，schema 2 缺字段补齐并保留未知键；单人 self 映射顶层。票据不缓存到磁盘，4600x 失效不自动重放。CLI 增加 wx 命令，GUI 增加微信授权页；同配置网络操作通过 `operation_lock.py` 非阻塞互斥。GUI 工作线程不读取 Tk 变量，失败后也回读旋转后的凭据。导出采用递归敏感字段清理。

默认依赖从零运行依赖变为 PyCryptodome，PyInstaller 收集 Crypto 子模块与动态库；Windows PE 与 CLI 版本均为 1.1.0。桌面不依赖 Android/ESP32，也不因本次变更移动已有 0.7.0 标签。实施范围及未完成的真实账号验收见[计划](desktop-port-plan.md)与[验证](../../desktop/docs/verification.md)。

以下为 1.0.0 历史设计；其中“零依赖”、接口无需微信票据等不再描述当前行为。

# 桌面版架构与构建（开发文档）

面向想读懂或改造 `desktop/` 单文件版的人。协议细节不在此重复，见
[api-contract.md](api-contract.md)；用户向说明见 [`desktop/README.md`](../../desktop/README.md)。

## 1. 代码地图

```
desktop/
├─ mooncheck.py                  源码直跑入口（sys.path 注入 src/）
├─ src/mooncheck_app/            包（PyInstaller 入口 __main__.py）
│  ├─ aes.py          纯 Python AES-128-CBC + PKCS7 + encrypt_cas_password；
│  │                  self_test() 为 FIPS-197 官方向量（key=000102...0f,
│  │                  plain=00112233..ff → 69c4e0d86a7b0430d8cdb78070b4c55a）
│  ├─ paths.py        app_dir()：frozen→dirname(sys.executable)，否则 repo desktop/；
│  │                  config_path()：env MOONCHECK_CONFIG 优先
│  ├─ config.py       v1.0.0 schema：DEFAULTS / load(create) / save(原子写+posix 0600) /
│  │                  get_value / set_value(点号路径+类型 coerce) / validate /
│  │                  masked_value / mask_url；PUT_ 前缀与 ^20\d{2}X+$ 视为占位；
│  │                  多账号：ACCOUNT_TEMPLATE(_virtual 合成单人视图)、effective_accounts、
│  │                  save_account_session/result、account_location、in_window(schedule)；
│  │                  推送：default_channel(顶层 notify 合成 label='')、iter_channels、
│  │                  channel_matches(空/all 收全部，否则定向)——与固件
│  │                  AppConfig::notifyChannelMatchesAccount 同规则
│  ├─ http_client.py  urllib 手工重定向、gzip/deflate、域匹配 CookieJar；UA 可配
│  ├─ cas_auth.py     EP01 取 execution+pwdEncryptSalt → 注入 BFP cookie →
│  │                  EP02 提交（classify_login 识别 MFA：authLogin/isMultifactor/
│  │                  「二次验证」字样）→ follow ticket → SESSION
│  ├─ checkin_api.py  EP03 预热 /api/isLoginFailure（关键，缺了必 4001）→
│  │                  EP04 进微应用 → EP06 today / EP10 checkin / EP08 locations /
│  │                  EP07 schedule / EP09 reverse；build_payload 与固件逐字段一致
│  │                  （含 locationEnabled:false 的欺骗字段、clientTime 格式）
│  ├─ push.py         通知渲染与发送（镜像固件 NotifyService::render 三 style）：
│  │                  0=GET 模板 {title}{body}/无占位符追加 query（serverchan 用 desp）；
│  │                  1=POST 文本（serverchan 降级 form-urlencoded title=&desp=）；
│  │                  2=POST JSON（URL 内嵌模板正则命中则按模板构造，否则
│  │                  {title,desp|body,device}）；is_server_chan(ftqq.com/ft07.com/.send)、
│  │                  _business_error(2xx 但 JSON code/errcode≠0)；
│  │                  fan_out(events,only_default,skip_default)——逐账号事件进关联通道、
│  │                  汇总只进默认通道，与固件 NotifyTarget 语义一致
│  ├─ service.py      编排 + Result(kind∈ok|warn|fail|mfa|not_configured)→exit 0|1|2|3|2；
│  │                  多账号：build_client(acct 视图覆盖 session/bfp)、_login_chain、
│  │                  _ensure_session(probe 失败自动重登)、_checkin_one(判重→坐标→
│  │                  可选 reverse→submit→复查→save_account_result)；
│  │                  窗口闸门：do_checkin/do_auto 非 --force 且 !in_window → warn 拒发
│  │                  （dry_run 不受闸门）；推送编排 _push：单账号 All 语义、
│  │                  多账号逐账号事件 skip_default + 汇总 only_default
│  ├─ cli.py         argparse：gui|selftest|status|login|checkin|auto|config|
│  │                  account(list/add/rm/on/off)|notify(list/set/add/rm/on/off/link/test)|
│  │                  sched(show/set)；login/status 带 -a；checkin 带位置参数 label；
│  │                  account add 的 lat/lng/addr 落账号自己的 location（模板 None 需
│  │                  isinstance 判断后再建 dict——setdefault 对已存在的 None 键无效）；
│  │                  注意 config 子命令不依赖配置文件存在（分派在 load 之前）
│  ├─ gui.py          tkinter Notebook 四页签（基本/账号/推送/打卡时间）+右侧日志；
│  │                  改动进 self.work 内存副本，【保存配置】过 validate 后落盘；
│  │                  工作线程 + queue 轮询（root.after 80ms）；表单留空=不改动；
│  │                  结果回流后若 config.load()!=self.cfg 则重载表单（会话/last_* 同步显示）；
│  │                  导出副本=顶层+全部账号 password→PUT_、session→空、url 脱敏
│  └─ __main__.py     无参数→（Windows 隐藏控制台）→GUI；有参数→CLI
├─ tests/test_offline.py         23 个用例，全程离线（pycryptodome 在则做逐字节交叉）
├─ tests/wsl_gui_smoke.sh        Linux 产物 GUI 冒烟（timeout 判活）
├─ mooncheck.spec                onefile；console=True（GUI 启动时自行隐藏控制台）
├─ build_windows.bat             conda env mooncheck-desktop + PyInstaller
├─ build_linux.sh                venv + pip requirements-build.txt + PyInstaller
└─ config.example.json           占位模板（PUT_* / SESSION=；含 schedule/accounts/notify 示例）
```

## 2. 设计要点与取舍

- **零运行时依赖**：AES 自实现替代 pycryptodome（逐字节对照测试兜底），GUI 用标准库
  tkinter。好处：解释器环境随便换、打包体积小、审计面小。
- **移植来源**：`dev/tools/pc_reference/wsc_{http,cas,checkin}.py`（协议等价参考实现）。
  桌面版把 print 改为 log 回调、CookieJar 补了域匹配、classify_login 从 pc_reference
  移植并扩展（它识别 MFA 页跳转）。协议若变更，两处都要跟。
- **多账号/推送 = 固件语义的桌面移植**（随固件 0.4.0 引入，桌面首发 v1.0.0 即含）：
  - 通道索引：默认通道（固件=notify.set 那条 / 桌面=顶层 notify）恒收全局事件与
    多人汇总；额外通道（≤5）只收 `accounts` 关联的逐账号结果，`all`=全收但不收汇总
    （汇总走 DefaultOnly）。
  - 去重：同固件「账号+日期+事件类型」一天一条；失败按轮次（`--force`/多轮才累加）。
  - 单人模式：`accounts=[]` 时合成 `_virtual` 标签 `self` 的顶层视图，会话/结果写回
    顶层字段——config 文件形态与 1.0 兼容。
  - style 编号 0/1/2、`gap/quota` 字段保留但桌面为同步发送（无队列），quota 不做
    每日限流（一次运行最多 账号数+1 条）。
- **与固件版其余差异**：不做 BLE/时钟自走/失败重试轮（计划任务多次调用即等价重试）；
  每账号会话独立存 config（固件存 NVS 账号表）。
- **PyInstaller 关键事实**（实测）：
  - onefile 引导器在**子进程退出前不读 config**——`sys.executable` 即 exe 路径，
    `paths.app_dir()` 靠它实现"配置与 exe 同目录"；源码运行落 `desktop/`，
    两种形态互不干扰。
  - 打包必须平台匹配（不能交叉编译）→ Linux 产物只能在（WSL）Linux 里做。
  - onefile 需要可写的临时目录（默认 TMP/TEMP；受限沙箱下设 TEMP 到工作区内再测，
    真实桌面用户无此问题）。
  - console=True + 启动时 `ShowWindow(GetConsoleWindow(), SW_HIDE)`（仅 frozen 且无参数），
    保住 `python mooncheck.py <cmd>` 的管道与退出码；`nuitka` 一类方案未评估。
  - UPX 不装：避开杀软误报放大，代价约 +3 MB。
- **退出码约定**（供计划任务判断）：0 成功/已打过 · 1 可重试异常/不在窗口 ·
  2 硬失败/配置不全 · 3 需要 MFA。

## 3. 测试

```
python -m unittest discover -s tests -v        # 离线，任何机器可跑
conda run -n mooncheck-desktop python -m unittest discover -s tests   # 含 pycryptodome 逐字节交叉
```

`tests/test_offline.py` 覆盖：AES 官方向量、CAS 密文 16 对齐、（可选）第三方库对照、
config 生命周期/校验/保留未知键、payload 13 字段、端点 URL；
多账号组（_virtual 会话写回顶层、账号校验与坐标覆盖、channel_matches 定向/全收、
in_window 边界）；push fan-out 路由（猴补丁 http_client.HttpClient 断言默认/额外各收几条、
serverchan desp vs 非 serverchan body）；窗口闸门（do_checkin/do_auto 拒绝、dry-run 旁路）；
CLI 子进程（init→set→account add/rm/off→notify set/add/link→sched set→auto 拦截→
selftest 汇报）。子进程断言用共享 per-test 临时 config 且以 UTF-8 解码
（Windows 父进程默认 GBK 会炸 reader 线程）。GUI 冒烟另行：构建四页签、注入
2 账号+1 通道，选中回填、应用坐标覆盖/多选关联、`_collect()` 无 error。

## 4. 脱敏约定

桌面版所有示例/默认值必须保持脱敏：学号占位 `PUT_YOUR_STUDENT_ID`、指纹占位
`PUT_32_HEX_FINGERPRINT`、密码占位 `PUT_YOUR_PASSWORD`、会话占位 `SESSION=`、
SendKey 类通道 url 用 `PUT_*` 占位、坐标 = 平台公布的打卡点中心（公开值，与隐私无关）。
真实值只进本地 `config.json`（已被 `.gitignore` 排除），永不入库。规则详见 [../README.md](../README.md)。

## 5. 与维护脚本的关系

`dev/tools/sync_to_mooncheck.py` 只同步 `firmware/ tools/ docs/` 三个目录——
`desktop/` 完全由本仓库直接维护，不参与私有工程同步；改动直接在 `mooncheck/desktop/`
里做即可。
