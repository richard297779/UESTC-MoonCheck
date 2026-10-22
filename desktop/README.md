# 🖥️ MoonCheck 桌面版（Windows / Linux）

固件版的同款打卡核心，装进**一个可执行文件**：不用装 Python、不用烧 ESP32，
在电脑上直接登录 CAS 并提交每晚定位打卡。适合"电脑忘不带/宿舍没插座"时应急，
或者你本来就想用电脑自动打卡。

- 单文件：`MoonCheck.exe`（Windows）/ `MoonCheck`（Linux x86-64），双击无参数 = 图形界面。
- 配置存在**可执行文件同目录**的 `config.json`（首次运行自动生成模板），改配置可以走命令行，也可以走界面。
- 会话 Cookie 自动缓存与续登；浏览器指纹配好后免二次验证——规则与固件版完全一致。
- **多账号**：最多 6 个舍友一起打，每个账号独立凭证 / 指纹 / 会话 / 坐标 / 今日结果。
- **打卡时间窗口**：可设 `22:00–23:30`，窗口外拒绝提交（`--force` 才可越窗），和固件版同一语义。
- **推送通知**：Bark / ntfy / Server酱 三类通道，可按账号关联——每个人的结果发给每个人，
  多人时默认通道只收一条汇总（与固件版 fan-out 规则一致）。

> 与固件版二选一整晚自动化：同一账号两边共用会话，同开会在 22:00 撞车互相挤掉线。
> 只用于本人账号；多人模式必须每位账号持有者知情并自己提供指纹。

---

## 1. 五分钟跑起来

1. 拿到可执行文件（自己构建，见 §5；本仓库不发布二进制）。
   有 Python 3.10+ 的也可以不构建、直接跑源码：`python mooncheck.py`（在本目录下）。
2. 双击运行 → 图形界面打开，同目录出现 `config.json`。
3. 在【基本】页填三项（学号 / 密码 / 浏览器指纹），坐标默认是平台公布的打卡点中心——在围栏
   （约 130 m）内能用，但在宿舍实测一次更稳：
   - **指纹**：Chrome 登录一次 `https://idas.uestc.edu.cn/authserver` 后，F12 →
     Application → Cookies → `https://idas.uestc.edu.cn` →
     `MULTIFACTOR_BROWSER_FINGERPRINT` 的值（32 位十六进制，HttpOnly，
     `document.cookie` 里看不到）。取法与截图见 [快速上手 §1](../docs/quickstart.md)。
   - **坐标**：界面里勾"打卡前校准地址"用浏览器实测最省事；手工取法见
     [快速上手 §2](../docs/quickstart.md)。
4. 点【登录】——看到"登录成功，会话已写入 config.json"即打通；点【查状态】看今日记录。
5. 【打卡时间】页确认窗口（默认 22:00–23:30），到点后点【打卡】。
   不确定载荷对不对？先勾"打卡仅预览"，不联网打印将要提交的内容。
6. 多人：在【账号】页逐个添加（每人自己的学号/密码/指纹，可选各自的宿舍坐标）；
   【推送】页配 Server酱/Bark/ntfy，并把通道关联到对应账号。

命令行等价（和界面完全等价，随便用哪个）：

```
MoonCheck.exe config set student_id 你的学号
MoonCheck.exe config set password              # 回车后隐藏输入
MoonCheck.exe config set browser_fingerprint 32位十六进制
MoonCheck.exe login
MoonCheck.exe checkin --dry-run                # 离线预览
MoonCheck.exe checkin --reverse                # 打卡前用当前会话校准地址
MoonCheck.exe status                           # 会话失效自动重登
```

多人 + 推送 + 窗口：

```
MoonCheck.exe account add A01 学号1 -            # 密码隐藏输入 指纹可后补
MoonCheck.exe account add A02 学号2 - 32位指纹 lat=30.75 lng=103.93 addr=某宿舍
MoonCheck.exe sched set 22:00 23:30
MoonCheck.exe notify set url=https://sctapi.ftqq.com/XXXX.send style=2 on=1
MoonCheck.exe notify add a02push url=https://sctapi.ftqq.com/YYYY.send accounts=A02
MoonCheck.exe notify test chan=a02push           # 单发一条测试
MoonCheck.exe auto                               # 窗口内逐个账号打卡+推送
```

## 2. 命令行一览

| 命令 | 说明 |
| --- | --- |
| `MoonCheck`（无参数） | 打开图形界面 |
| `selftest` | 离线自检：加密正确性 + 配置校验 + 打卡窗口 + 每账号载荷 + 每通道渲染，不联网 |
| `status` | 今日打卡状态；会话失效自动重登；多人逐个账号汇报 |
| `login [-a 标签]` | 强制重新登录（换指纹后、或想刷新会话时用） |
| `checkin [标签] [--dry-run] [--force] [--reverse]` | 单账号打卡（省略标签：单人模式打默认账号，多人模式等同 `auto`）；`--force` 同时是"已打过重打"与"越窗强打" |
| `auto [--force]` | 全账号轮：窗口闸门 → 逐账号检查/提交 → 每人一条推送 + 默认通道一条汇总 |
| `sched show` / `sched set <22:00> [<23:30>]` | 查看/设置打卡时间窗口 |
| `account list` / `account add <标签> [学号] [密码] [指纹] [lat=.. lng=.. addr=.. enabled=0]` / `account rm\|on\|off <标签>` | 账号表管理（最多 6；标签规则同固件 `A01`） |
| `notify list` | 通道总览（默认 + 额外，含各自今日已发数） |
| `notify set url=.. [style=..] [on=..] [gap=..] [quota=..]` | 默认通道（收全局事件与多人汇总） |
| `notify add <标签> <url> [style=..] [on=..] [accounts=A01,A02\|all]` | 额外通道：只收关联账号的逐账号结果；不写 accounts 默认全收 |
| `notify rm\|on\|off <标签>` / `notify link <标签> <A01,A02\|all>` | 通道管理 |
| `notify test [chan=标签] [文本]` | 立即发一条测试（默认发选中/首个启用额外通道，无则默认通道） |
| `config show [--all]` / `config set <路径> [值]` / `config init [--force]` / `config path` | 同旧版；`notify.url` 显示时脱敏 |
| `-v` / `--config <文件>` | 详细日志 / 临时指定配置文件 |

退出码：`0` 成功/跳过 · `1` 可重试警告（含"不在窗口内"） · `2` 明确失败/配置不全 · `3` 需要二次验证。
挂到计划任务时用 `MoonCheck auto`——窗口闸门在程序里，定时器只需每天 22:00 之后调用即可；
提前调用会被拒绝并返回 `1`，防止误打。

## 3. 图形界面

四个页签 + 右侧操作日志；所有改动要点【保存配置】才落盘（有 error 会拦截并提示）：

- **基本**：默认账号三项凭证、默认位置、超时、会话 Cookie。
- **账号**：左表右表单——【新增】/【应用修改】/【删除】/【启用禁用】；
  勾"该账号使用自己的坐标"可给每个人单独设 lat/lng/地址（不勾=用默认位置）。
- **推送**：默认通道（开关/url/风格）+ 额外通道表——url/风格/**关联账号多选**
  （Ctrl/Shift 点选；勾"收所有事件"等同 all）；【发送测试】即时验证某条通道。
- **打卡时间**：`checkin_at` / `window_end` 两个输入，实时提示"现在在不在窗口内"。
- 底部动作区：【保存】【自检】【登录】【查状态】【打卡】【导出配置副本】（打码）【打开配置目录】。
  勾"打卡仅预览"= `--dry-run`；MFA 拦截会弹窗说明处置办法；关窗口有未保存修改会提示。

## 4. 想每晚自动？

程序本身不做常驻调度，交给系统定时器即可（会话过期自动重登，无需每天登录）：

- **Windows**：任务计划程序 → 每天 22:02 启动程序
  `MoonCheck.exe auto`（加 `-v` 输出重定向到日志文件即可留痕）。
- **Linux**：`2 22 * * * /path/to/MoonCheck auto >> /path/to/mooncheck.log 2>&1`。
  假期/周末服务端返回"无生效时段"，程序会如实跳过，不会误打。
- 多人：每人**当天第一次成功**后不再重复提交（去重按账号+日期），
  失败按轮次重试即可，通道侧还有一天内每账号只发一条"成功"的去重。

## 5. 自己构建可执行文件

运行时零第三方依赖（AES 为纯 Python 实现、界面用标准库 tkinter、推送走同款 HTTP 客户端）；
**只有打包**需要 PyInstaller。

**Windows（一条命令）**：装好 Miniconda 后运行本目录的 `build_windows.bat`，
产物在 `dist/windows/MoonCheck.exe`。脚本会创建/使用 conda 环境 `mooncheck-desktop`
（Python 3.12 + PyInstaller），不污染你的基础环境。

**Linux（推荐在 WSL 里做）**：

```
wsl                                        # 进入 Ubuntu
sudo apt install -y python3-venv python3-tk squashfs-tools
cp -r /mnt/c/.../mooncheck/desktop ~/mooncheck-build && cd ~/mooncheck-build
bash build_linux.sh                        # 产物在 dist/linux/MoonCheck，约 14 MB
cp dist/linux/* /mnt/c/.../mooncheck/desktop/dist/linux/
```

要点：① 必须在**目标平台**上打包（PyInstaller 不能交叉编译，Windows 上做不出 Linux 产物，
反之亦然）；② 源码先拷进 WSL 的 ext4 文件系统再构建（跨 `/mnt/c` 慢一个数量级）；
③ `squashfs-tools` 是 PyInstaller onefile 的打包依赖，`python3-tk` 保证头文件齐全；
④ 构建出的产物与在本机 Linux 上跑的完全一致（GUI 在 WSLg 下实测可开窗口）。

## 6. 常见问题

- **双击没窗口**：同目录生成 `config.json` 需要 2–5 秒（onefile 自解压），别连点。
  被杀软拦截时把文件加入信任——纯 Python + PyInstaller 产物误报常见，源码在这里，可自行验证。
- **`[MFA]` 二次验证**：指纹没配/不是已信任浏览器的值。按提示取指纹后
  `MoonCheck.exe config set browser_fingerprint ...`（多人：`account add A01 学号 - 指纹`）再 `login`。
- **`NETWORK_ERROR`**：不在校园网/没有校园网出口。桌面版和固件版一样，打卡必须走
  校园网链路（或自己搭 VPN/代理到校园网）。
- **`[WARN] 不在打卡窗口内`**：正常保护。到点再跑，或明确要越窗时加 `--force`。
- **收到重复推送？** 同一账号一天只会推一条"打卡成功"；"今日已打卡"的跳过轮不推。
  汇总只在默认通道，逐账号结果只进关联通道——见 §1 多人示例。
- **`[NEED]` 配置不全**：按明细补齐（`config show` 看当前值）。
- **换电脑/发配置**：用【导出配置副本】拿打码版；密码与会话永远不要发出去。

## 7. 安全与边界

- `config.json` 含明文密码与有效会话（多人模式下是**每个人**的），等同全宿舍的通行证：
  Linux 下自动 0600，Windows 下请放在自己的用户目录，别同步到网盘，别提交进 git
  （本目录 `.gitignore` 已排除）。
- 频率纪律：只在每晚窗口登录一次，失败才重试。白天反复 `login` 会挤掉手机 App 会话。
- 同一账号同一时间只跑一个自动化实例（桌面版或固件版二选一）。
- 推送通道存的是第三方服务地址（含 SendKey 这类密钥），与密码同级对待；
  界面与 `config show` 都会脱敏，只有 `--all` 打码。

*相关文档：[快速上手](../docs/quickstart.md) · [通知推送（固件版同款通道）](../docs/notify.md) ·
[多账号（固件版同款表）](../docs/multi-account.md) · [FAQ](../docs/faq.md) ·
[指令手册（固件版）](../docs/commands.md) · [架构与构建细节（开发）](../dev/docs/desktop-app.md)*
