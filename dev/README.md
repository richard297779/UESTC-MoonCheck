# MoonCheck 开发者文档

想**使用**这个项目（刷固件、配置、打卡）请直接看 [README](../README.md) 和 [用户文档](../docs/)。

这里放的是写给**想读懂原理、复刻链路、二次开发**的人的内容：逆向过程、接口契约、固件设计、实验工具。
都是脱敏后的版本，可以放心看、放心改。

> 固件源码注释里写的 `docs/api-contract.md`、`tools/pc_reference/...` 等路径，是主开发工程里的位置；
> 在本仓库中，这些文档位于 `dev/docs/`，开发工具位于 `dev/tools/`，用户工具（`tools/ble_console.py` 等）位置不变。

---

工作区路径及缓存边界见[目录约定](docs/workspace-layout.md)；发行文件统一按版本归档，入口见[发行索引](../release/README.md)。

## 目录结构

```
mooncheck/
├─ README.md            用户入口
├─ docs/                用户文档（快速开始 / 刷固件 / 指令 / 通知 …）
├─ firmware/            固件源码（PlatformIO）
├─ desktop/             桌面单文件版源码 + 构建脚本（Windows/Linux）
├─ android/             原生 Android BLE 控制端、测试与构建工程
├─ tools/               用户工具：ble_console.py、notify_sink.py …
├─ dev/                 ← 你在这里
│  ├─ docs/             开发文档：协议、设计、验证记录
│  └─ tools/            开发工具：抓包分析、TLS、诊断脚本、PC 参考实现
└─ release/             发布产物与发行说明
```

## 文档索引

v0.7.0 多板型实现与验收边界见[扩展方案](docs/board-expansion-plan.md)及[板型矩阵](../docs/board-support.md)。APK 新增运行状态与开发者脱敏日志；新增型号未经实际验证。

| 文档 | 内容 |
| --- | --- |
| [api-contract.md](docs/api-contract.md) | 历史平台接口抓包（EP01–EP10）及当前微信操作票据补充 |
| [ble-protocol.md](docs/ble-protocol.md) | BLE 帧格式、历史设计、当前 JSON 指令与事件、LED/按键、错误码 |
| [firmware-design.md](docs/firmware-design.md) | 固件早期设计及当前微信授权 / 票据实现说明 |
| [python-test-design.md](docs/python-test-design.md) | PC 参考实现的设计（模块划分、CLI、错误码、测试） |
| [python-verification-plan.md](docs/python-verification-plan.md) | V0–V5 验证方案与实测结论 |
| [test-plan-checkin-path.md](docs/test-plan-checkin-path.md) | 「能登录却打不了卡」的完整排查记录与二分定位过程 |
| [capture-guide.md](docs/capture-guide.md) | 用 Burp + DevTools 抓包的步骤、CA 证书、脱敏要求 |
| [multi-user-design.md](docs/multi-user-design.md) | 多账号表（最多 6 条）与逐个开关的设计 |
| [notify-design.md](docs/notify-design.md) | 通知通道格式、事件表、限流与失败隔离 |
| [network-uestc-wifi.md](docs/network-uestc-wifi.md) | 校园网 Portal / 802.1X 的判定与现状 |
| [desktop-app.md](docs/desktop-app.md) | 桌面单文件版：代码地图、schema、与固件语义的对应 |
| [Android 实施指南](../android/docs/implementation-guide.md) | APK/小程序取舍、BLE 契约、目录规划与验收步骤 |
| [项目体检记录](../docs/project-health.md) | v0.6.0 检查范围、自动化证据与发布限制 |
| [release-guide.md](docs/release-guide.md) | 发布者手册：锁版本、merge_bin、GitHub Release 流程 |
| [screenshots.md](docs/screenshots.md) | 用户文档的配图清单（维护用） |

## 工具索引

**抓包与分析**（对浏览器导出的 HAR 使用，只输出结构，不打印 Cookie/token 值）

| 工具 | 用途 |
| --- | --- |
| `dev/tools/analyze_har.py` | 按时间序列出 HAR 全部请求，快速看清链路 |
| `dev/tools/show_capture_item.py` | 查看单条抓包的结构与请求头（脱敏） |
| `dev/tools/extract_app_urls.py` | 从 HAR 里挖出「晚安成电」的真实启动地址 |
| `dev/tools/extract_portal_sequence.py` | 提取「登录后 → 点图标前」的请求序列，生成可回放清单 |
| `dev/tools/inspect_app_js.py` | 从 JS 响应里找「点击图标」的代码路径 |
| `dev/tools/compare_cookies.py` | 对比两个域上的 Cookie 名单（只列名字） |

**TLS / 证书 / 接口试验**

| 工具 | 用途 |
| --- | --- |
| `dev/tools/check_tls.js` | 检查目标域名的 TLS 握手与证书链 |
| `dev/tools/gen_ca_pem.js` | 生成内置信任锚 `public_ca_pem.h` |
| `dev/tools/gen_public_ca.js` | 生成公开根证书 PEM（供设备校验推送域名） |
| `dev/tools/verify_public_ca.js` | 用这份 PEM 复验各推送域名的真实握手 |
| `dev/tools/probe_cas_crypto.js` | 对照 CAS 登录页 JS，验证密码加密算法 |
| `dev/tools/extract_burp.js` | 从 Burp 导出内容里提取请求 |
| `dev/tools/fetch_asset.js` | 抓取并保存前端静态资源 |

**PC 参考实现**（`dev/tools/pc_reference/`，纯标准库 + pycryptodome）

| 文件 | 用途 |
| --- | --- |
| `wsc_http.py` / `wsc_cas.py` / `wsc_checkin.py` | 最小 HTTP 客户端、CAS 登录、打卡接口封装 |
| `verify.py` | 命令行验证工具，`selftest / status / login / checkin` |
| `diag_*.py` | 一组受控实验：授权延迟、会话绑定、Referer、门户预热二分、TTL 等 |
| `recon_mfa.py` | 触发并测绘二次验证页面（只用于研究） |
| `companion_probe.py` | 用 Playwright 走真实浏览器登录，做对照实验 |
| `portal_sequence.json` | 已脱敏的门户请求序列（`_t=<SN>`，无 Cookie） |

**发布**：`dev/tools/make_release.bat`（双击）/ `make_release.ps1` —— 读统一 VERSION、核对固件/APK 版本和新鲜度、
`esptool merge_bin` 打包、生成整套 ZIP/清单/SHA256；默认不上传，显式 `-Upload` 用 `gh` 上传并校验后发布。

**用户工具**在 [`../tools/`](../tools/)：`ble_console.py`（蓝牙配网台）、`notify_sink.py`（局域网通知中转）、
`board_id.py`、`clear_ble_cache.ps1`。

## 复刻路线

1. [快速开始](../docs/quickstart.md) 走一遍，先让设备跑通；
2. 想知道每一步在干什么 → [api-contract.md](docs/api-contract.md)；
3. 想自己写一套 PC 实现 → [python-test-design.md](docs/python-test-design.md)，代码看 `dev/tools/pc_reference/`；
4. 想改固件 → [firmware-design.md](docs/firmware-design.md) + [ble-protocol.md](docs/ble-protocol.md)，编译看 [firmware/README.md](../firmware/README.md)；想改桌面版 → [desktop-app.md](docs/desktop-app.md)；
5. 想抓自己的包复现结论 → [capture-guide.md](docs/capture-guide.md)。

## 脱敏约定

仓库里的示例值都是占位，替换规则如下：

| 类型 | 仓库中的值 | 说明 |
| --- | --- | --- |
| 坐标 | `30.75498, 103.933883` | 平台公布的打卡点中心，**不是**任何人的宿舍坐标 |
| 浏览器指纹 | `0123456789ABCDEF0123456789ABCDEF` | 32 位十六进制占位 |
| 学号 / 密码 | `2023XXXXXXXXX` / `your-password` | 一律占位 |
| 串口 | `COM6` / `COM7` | 只是 PlatformIO 环境名 |
| 通知 token | `AbCdEf123456`、`wanan-7f3k9x2m` | 演示用假值 |

提交前请自查：真实坐标、真实指纹、学号、Cookie、token、本机绝对路径（`C:\Users\...`）都不应出现。
截图里的这些内容也要打码，见 [screenshots.md](docs/screenshots.md)。

## 维护者说明

- 公开副本由私有主工程用 `tools/sync_to_mooncheck.py` **单向同步**（该脚本不外发）；
- `dev/docs/`、`dev/README.md`、`docs/`、各 README 为手工维护，同步不会覆盖；
- 同步后跑一次仓库卫生检查（`check_repo_hygiene.py --strict`，同样只在私有主工程里）；
- 新增开发文档请放 `dev/docs/`，并在上面的索引表里加一行。

## 许可

MIT，见 [LICENSE](../LICENSE)。
