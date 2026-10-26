> 发行验收更新：2026-10-27 用户确认修正版“实测可行”并授权发行；未指明本次具体板型、手机或逐项测试环境，不据此声称两个原有目标均逐板验证。新增四种板型仍未经实际验证。

# v0.7.0 实施更新（2026-10-27）

已实现：六种板型配置、独立产物、单核适配、板型元数据交叉校验，以及 APK 运行状态/开发者日志。运行指标使用设备端分配器，支持旧协议固件缺失字段；日志限量、脱敏、不持久化，断线或退出后清空。新增板型未经实际验证，维护者只有 S3-N16R8 与 WROOM-32 可实测；用户已确认修正版实测可行，具体测试板型未说明。详细[板型支持](../../docs/board-support.md)及[扩展方案](../../dev/docs/board-expansion-plan.md)。

以下保留原始设计与历史验收记录，历史通过结论不代表本版新功能已实测。

# Android BLE 控制端实施指南

日期：2026-10-26。代码基线：`3465b40`；固件 `0.5.1`，协议 `1`。
本文件是仓库实施方案，由代理依据用户“比较并确定方案”的授权选定路线；尚无手机实测结果。实施时应重新核对协议与依赖版本。

实施更新（2026-10-26）：Android 工程、Compose 页面、BLE 通信与本地测试已建立，构建方法见上级 README。本文保留最初路线与验收要求；实际完成的检查与限制见 `verification.md`。

联调更新（2026-10-26）：用户确认 APK 实际联调通过、功能全部正常，v0.6.0 转为正式 Latest。

发行更新：项目与 Android 现统一为 v0.6.0，版本由根目录 VERSION 管理；最新检查与发行边界见[项目体检](../../docs/project-health.md)。

## 1. 路线决定

首版采用 **Kotlin + Jetpack Compose 原生 Android APK**，使用 Android BluetoothGatt API、协程和 StateFlow；应用作为 ESP32 的近距离配置与控制器。每天调度、登录、取票和提交仍由固件完成，手机无需常驻或保持连接。

不将 Python 打包进 APK，也不把桌面版业务引擎移植到手机。复用的是控制台的 JSON 指令语义，而不是 Python 的终端输入解析与 Windows 重试策略。

| 考量 | 原生 Android APK | 微信小程序 |
| --- | --- | --- |
| 便捷操作 | 首次安装后可直接打开，表单与按钮完整覆盖 BLE 配置 | 用户打开方便，适合少量快捷按钮 |
| BLE 调试与兼容 | 可直接控制 GATT 队列、MTU、回调和生命周期 | 还要适配微信 API、基础库和容器生命周期 |
| 首版范围 | 聚焦安卓用户，与当前请求吻合 | 跨端有潜力，但仍需分别做真机验证 |
| 发布 | 可提供签名 APK 下载，用户允许该来源安装 | 正式分发需核实小程序账号、类目、隐私声明及审核要求 |
| 微信凭据 | 仍需按现有流程准备本人授权信息 | 自建小程序不因此获得学校小程序的授权信息 |
| 当前选择 | **主路线** | 留待 APK 验证后按需求评估 |

选择依据是当前工程规模和 BLE 可控性，不是“小程序不能做蓝牙”。首版不引入 Flutter/React Native/uni-app：目前没有同时交付多个平台的要求，额外插件层收益有限。这是工程判断，不是平台性能结论。

边界：APK 首版不包含手机独立打卡、云服务器、后台常驻、OTA、自动抓取浏览器 Cookie 或微信凭据；用户继续参考现有快速开始准备凭据。先解决“配置和按键控制便捷”，不要把“一键控制”承诺成“一键取得全部授权”。

## 2. 代码事实与迁移注意点

核对依据：`tools/ble_console.py` 的 `Console.send`、`_on_notify`、`run_command`；`firmware/src/ble_service.cpp` 的 `begin`、`handleIncoming`、`notifyLine`；`firmware/src/command_router.cpp` 的 `dispatchCommand`、`replyOk`、`replyError`。

- 控制台是“输入 → JSON → NUS RX 写入 → NUS TX 通知 → 显示”的适配层，手机用表单直接构造 JSON，可保留 SSID/密码中的空格，不再按终端空白切词。
- 当前控制台整条报文一次写入且 `response=False`，随后固定等待 150 ms；这不能作为手机长报文与业务成功判据。
- 固件按换行收集输入，缓冲上限 4096 字节，允许一条 JSON 拆为多次写入。
- 后续实现审阅发现基线固件的命令任务项仅有 512 字节，完整 JSON 实际上限为 511 字节。本次已改为有界堆分配传递完整命令，避免截断，并通过 `hello.maxCommandBytes=4095` 声明能力。没有该字段的固件按 511 字节处理，不能仅依据 BLE 缓冲大小推断长命令可用。
- 回执以 `evt:"ok"` / `evt:"error"` 表示结果，携带请求 `id`；其他 `progress` / `result` / `alert` 可能不带 ID，应作为设备事件展示。
- `hello` 有 `fw`、`proto`、`btMac`、`mac`、`caps` 等字段。但当前 `caps` 不列出全部微信操作，不可据此隐藏所有微信功能；当前 `hello.bleName` 也不是读取实际自定义广播名，设备卡片应保留扫描名称，以 `btMac` 辅助确认身份。
- 固件 RX 支持 Write 和 Write Without Response。首版优先使用带响应写入并串行等待回调，吞吐并非本项目瓶颈；GATT 写入回调成功仍不等于命令执行成功。

## 3. BLE 契约：优先实现和验证

### 服务与连接

| 项目 | UUID / 规则 |
| --- | --- |
| Nordic UART Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| RX：手机 → 设备 | `6e400002-b5a3-f393-e0a9-e50e24dcca9e`，Write |
| TX：设备 → 手机 | `6e400003-b5a3-f393-e0a9-e50e24dcca9e`，Notify |
| CCCD | `00002902-0000-1000-8000-00805f9b34fb` |

连接状态机：权限就绪 → 限时扫描 → 用户选板 → 连接 → 服务发现 → MTU 协商 → 本地通知开启及 CCCD 写入成功 → hello 回执 → 可操作。

扫描默认 15 秒，按 RSSI 排序并显示广播名和设备地址；支持刷新、停止扫描。优先匹配 NUS 服务并结合 WANAN 名称识别，服务 UUID 不足以证明是 MoonCheck，须在握手后确认。当前实际广播数据包含服务 UUID，扫描响应包含名字；以实现为准，不依据文件中与其相反的旧注释。多板必须让用户明确选择，不能自动连接信号最强板。

所有 GATT 操作串行执行，包括 MTU 请求和 CCCD 写入；等待对应回调及超时后才推进。连接失败最多自动重试两次，释放旧 GATT，提供主动重试入口。用户断开与权限被撤销时停止重试。

### MTU 是首个验收门槛

请求 MTU 512，以 `onMtuChanged` 实际值为准。Android 14 起首次 MTU 请求行为有所变化，不能认为请求参数就是最终协商值。

当前固件 `notifyLine` 当 `min(MTU - 3, 480) <= 56` 时等待后暂存回执，即 **MTU 必须至少为 60** 才能正常进入其发送路径。即便很短的回执也会经过这个判断；保持默认 MTU 23 并非可用的“低速兼容模式”。

首版若实际 MTU < 60 或协商失败，显示“当前固件需要更大的蓝牙 MTU”，禁止进入控制就绪态并允许重连。先在两类板上验证 MTU 512/较小协商值的通知；若目标手机不能满足，后续另修固件低 MTU 发送策略，再扩展支持。向上行分成 20 字节并不能解决下行被暂存的问题。

### 上行编码

1. 用 JSON 序列化库构造对象，附单调递增正整数 `id`；每个连接会话独立管理 pending，忽略旧会话的回调。
2. 整条序列化文本加一个 `\n`，整体编码 UTF-8；总字节数（含换行）不得超过 4096。
3. 发送前还须检查 JSON 字节数不超过设备声明的 `maxCommandBytes`（缺失按 511 字节）。按字节切片，大小不超过 `min(实际MTU - 3, 480)`，必要时可保守减小；不得按字符数切片，也不得给每片加换行。
4. 每片等待写入回调，再写下一片。最后一片完成后继续等待同 ID 的业务回执。
5. 发送中断时不要在原连接上盲目重发整行：设备可能残留半条 JSON。断开重连清理 RX 缓冲，再由用户确认重试。

### 下行解码

先按字节缓存与换行切帧，再解析 JSON，避免依赖“一次回调恰好一帧”。短回执直接处理；长回执形如：

```json
{"seq":0,"of":2,"b64":"..."}
```

`seq` 从 0 开始。对每片 base64 解码为原始字节，按序拼接所有字节，**最后一次性 UTF-8 解码并解析 JSON**。Python 现有代码逐片 UTF-8 解码，遇到中文字符恰好跨分片边界可能产生替换字符，手机不能照搬。旧 `chunk` 字符串帧可作为兼容解码器保留，但必须有对应旧固件样本测试，不能宣称已验证。

当前分片外壳没有请求 ID 或消息组 ID。首版只允许一个在途业务请求，`seq=0` 开始新组；检查序号范围、总数一致、缺片、重复片内容冲突。建议接收重组上限 64 KiB、最多 2048 片、空闲 5 秒超时（客户端初始策略，待实测调整）。越界或冲突丢弃组并提示结果不完整，不能混拼成成功。

固件只在单次 `sendRaw` 内加锁，并未对整组长回执持锁；若后台事件穿插，只接收完整事件，不插入分片字节。两个分片组交错无法可靠恢复，应使当前结果失败并重新查询；出现实测证据后再考虑固件增加组 ID 和整组发送互斥。

### 命令完成与超时

成功回执示例：`{"id":1,"evt":"ok","msg":"hello","data":{...}}`；错误回执带 `evt:"error"`、`code`、`msg`。未知字段容忍，未知协议版本仅展示诊断并阻止写配置，不能凭版本号猜字段。

普通查询/设置初始超时 15 秒，联网操作 120 秒；`test.chain` 需按 `wait_s` 另加网络余量，多账号需按账号数量、间隔和网络耗时单独设置长超时。它们是初始客户端预算，不是设备完成时间保证。取消等待不代表固件任务被取消；超时显示“结果未知”，后续查状态确认。

不自动重试打卡、通知测试、账号增删、凭据导入、恢复出厂等可能已生效的操作。断线后重新握手、刷新状态，丢弃旧 pending；不要自动重放上次写操作。即便客户端单请求在途，设备的每日后台任务仍可能在执行，应展示 busy/进度而非连续排队点击。

## 4. 产品页面与按钮范围

| 页面 | 首版交付 |
| --- | --- |
| 设备 | 扫描、选板、连接状态、固件版本、MAC、断开 |
| 首页 | WiFi/时间/自动调度状态、刷新、体检、查看最近结果、手动打卡 |
| 配置向导 | WiFi → 账号/指纹 → 本人微信凭据 → 坐标 → 时间 → 逐项验证 → 用户开启自动模式 |
| 账号与授权 | 单人配置、最多 6 人账号列表、启停、凭据槽位状态与导入 |
| 诊断 | 时间、WiFi 扫描、网络检查、微信取票测试、链路自检、脱敏日志 |
| 高级 | 通知渠道、广播名、删除/重置确认；原始 JSON 控制仅开发版本考虑 |

按钮文案必须区分“体检”“微信取票测试”“提交打卡”。`test.chain` 及 `test.multi` 的演练仍可能登录；不能统称无副作用。手动打卡默认 `force=false`，`force` 放高级操作并确认。恢复出厂需明确说明擦除配置并输入 YES，发送 `confirm:"YES"`。

配置向导并非事务：每一步单独保存、等回执并标记成功/失败；中断后提示已经保存哪些步骤，不静默回滚或重新导入。自动模式开关不在未完成配置时擅自启用。

账号标签与微信槽位必须按固件账号表顺序明确显示，不能只用 UI 列表行号推断；账号删除/清空/替换后重新加载列表和 `wx.status`，核对映射并提示必要的重新导入，避免错用他人授权。

## 5. 首版指令映射

以下以当前源码为准，全部附加 `id`；可选字段省略不能随意改成空字符串。

| UI 操作 | op | 主要字段 |
| --- | --- | --- |
| 握手/刷新 | `hello` / `status` | 无 |
| 时间/日志 | `test.time` / `logs` | 日志 `n` |
| WiFi 扫描/保存 | `wifi.scan` / `wifi.set` | `ssid`, `pass` |
| 网络检查 | `test.http` | `url`，沿用控制台 generate_204 测试地址 |
| 保存单人账号 | `auth.set` | `user`, `pass`, `bfp?` |
| 保存坐标 | `loc.set` | `lat`, `lng`, `addr?` |
| 保存计划 | `sched.set` | `at`, `window_end`, `jitter_s`，可选 `window_start` |
| 自动模式 | `auto.set` | `enabled`，布尔值 |
| 授权状态/导入 | `wx.status` / `wx.import` | 导入 `slot`, `openid`, `access`, `refresh`, `expiresAt` |
| 微信取票测试 | `wx.test` | `slot` |
| 体检/链路 | `test.probe` / `test.chain` | 后者 `wait_s`, `force` |
| 手动提交 | `test.checkin` | `force:false` |
| 账号管理 | `account.list/add/enable/remove/clear` | 依操作使用 `label`, `user`, `pass`, `bfp`, `lat`, `lng`, `addr`, `on` |
| 多人演练 | `test.multi` | `probe:true`, `force:false`（窗口外有单独策略） |
| 通知管理 | `notify.status/set/add/enable/remove/link` | 以路由器和通知文档为准，独立开发步骤 |
| 广播名/重置 | `name.set` / `factory.reset` | `name` / `confirm:"YES"` |

表单校验：纬度 [-90,90]、经度 [-180,180]、有限数字；HH:MM 和时间窗口一致性；槽位 0–5；expiresAt 为非负 Unix **秒**，可用 0 请求设备首次使用前刷新；不能把 `expires_in` 剩余秒直接当绝对时间。字段长度与账号标签规则继续核对 `app_config.cpp`、`account_store.cpp`、`wx_credential_store.cpp`，在实现时形成同源约束表。

首版手动输入经过确认的宿舍坐标，不自动用手机实时位置覆盖设备坐标，避免引入未确认的坐标系转换和额外定位权限。微信凭据仅由本人按现有文档取得；新建微信应用的 AppID 和登录能力不能替代学校授权链。

## 6. Android 工程与目录

项目根目录固定 `android/`。以下为规划结构；当前已经实现工程，各文件以目录实际内容和 README 为准。

```text
android/
  README.md
  docs/implementation-guide.md
  settings.gradle.kts          # 实现阶段生成
  build.gradle.kts
  gradle/libs.versions.toml
  gradle/wrapper/              # Wrapper 随源码提交
  gradlew / gradlew.bat
  app/
    build.gradle.kts
    src/main/AndroidManifest.xml
    src/main/java/.../
      ble/                    # 扫描、GATT 状态机、串行队列
      protocol/               # 编解码、分片、命令、回执
      data/                   # DeviceRepository、会话状态
      ui/                     # Compose 页面与 ViewModel
    src/test/                 # JVM 协议及状态测试
    src/androidTest/          # 必要 UI/设备验证
```

先采用单个 app 模块与按包分层，避免过早拆成多个 Gradle 模块。ProtocolCodec 仅处理字节/JSON，无 Android 依赖；BleTransport 隐藏平台 API；DeviceRepository 管理请求与状态；ViewModel 管理页面交互，UI 不直接访问 GATT。

建议 minSdk 26（Android 8，首版支持范围选择，非 BLE 平台最低要求）；compileSdk/targetSdk、AGP、Kotlin、Compose 和 JDK 版本在工程初始化时按官方兼容矩阵锁定，并提交版本目录与 Wrapper。不要在本规划阶段虚构已验证的构建版本。

Android 12+ 申请运行时 `BLUETOOTH_SCAN`、`BLUETOOTH_CONNECT`；旧版按扫描要求申请定位权限，旧蓝牙权限设置 maxSdkVersion 30。若使用 `neverForLocation`，必须验证其过滤部分 BLE 广播的影响。首版不申请手机 WiFi 扫描、存储、后台定位；WiFi 列表是由 ESP32 `wifi.scan` 返回。无手机 HTTP 需求时不声明 INTERNET 权限。

旋转屏幕由 ViewModel 保持状态；页面关闭或退出应用释放 GATT。应用进入后台时默认终止本次控制会话，返回后重连刷新并标记未完成操作结果未知；是否短暂保活需后续另定，首版不建前台服务保证永久连接。

## 7. 凭据与交付约束

- App 默认不持久化学号密码、指纹、微信 token、SESSION、含密钥推送 URL；提交后清理敏感表单状态，不写 SavedStateHandle、日志、崩溃报告或备份。只记非敏感设备偏好；后续确需凭据存储再设计 Keystore 保护。
- 脱敏应覆盖请求、回执和固件日志；不能只隐藏 `wx.import` 三字段。Python 当前对其它命令未统一脱敏，不照搬其打印策略。首版不提供原始日志导出，后续须专门核验脱敏后才提供。
- 当前 BLE 服务没有显式配置配对/绑定/加密/应用鉴权；APK 不能消除固件链路的凭据暴露与未授权控制风险。本人受控近距离试用前说明该现状；面向广泛用户发布前，应验证并实现配对或设备授权机制、限制可配置时段，以及固件接收日志脱敏。若修改协议，先做版本兼容设计。
- 在授权导入页面说明“数据发送至所选设备”，显示设备身份；不把真实凭据放样例、测试夹具、截图、Notion 或仓库。
- Debug APK 用于开发测试；用户交付使用正式签名 release APK，提供版本、SHA-256 与支持的固件范围。同一应用更新须保留签名身份，keystore 和密码不入库。
- `android/app/build/outputs/apk/` 是构建输出路径，正式分发时可复制到被忽略的 `release/v<版本号>/`。本方案不执行发布、不创建签名密钥。

## 8. 实施顺序与完成标准

| 阶段 | 工作 | 完成标准 |
| --- | --- | --- |
| P0 连通验证 | 创建工程、权限、扫描、选板、MTU、订阅、hello/status | 两种板型真机握手；记录实际 MTU；模拟器截图不能替代 BLE 实测 |
| P1 协议核心 | 字节分片、重组、ID 匹配、业务超时、错误、断线恢复 | 长请求、中文长回执、错序/缺片/重复/超界都可判定；无自动重复提交 |
| P2 可用配置 | 向导、单人账号、授权导入、WiFi/坐标/时间、自动开关 | 重启后回读配置；手机断开后设备独立调度；敏感数据不落盘 |
| P3 完整控制 | 多账号、诊断、通知、危险操作确认 | 槽位映射可解释，体检与提交明确区分，失败不会显示成功 |
| P4 发布准备 | 兼容矩阵、链路保护、签名、脱敏审查、安装说明 | release APK 安装/升级通过；记录 SHA-256、固件与手机验证版本 |

建议首次实现先做到 P0–P2，验证后再扩展 P3。不要先铺满 UI 再处理长 token、MTU 与回执问题。

必须验证的用例：

1. JVM：JSON 特殊字符与空格、UTF-8 中文跨字节片、base64 原始字节重组、4096 边界、超限拒绝、ID 与事件分流、缺片超时、坏 JSON 与未知字段。
2. 状态机：GATT 回调失败/超时、CCCD 失败、低 MTU 拒绝、旧连接回调隔离、用户断开停止重试、配置半途断线结果未知。
3. 真机：至少 Android 11、12、14+ 的代表设备（至少两个厂商），分别覆盖 ESP32-S3 与 ESP32-WROOM-32；权限拒绝/撤销、蓝牙关闭、板子占用、多板选择、屏幕旋转、切后台、设备改名与重启。
4. 端到端：长授权导入使用本人授权的测试设备，在不导出真实值的情况下核验 `wx.status`；WiFi/配置保存回读；打卡或真实通知测试仅在操作者明确触发时执行，CI 不访问真实账号或平台。

常规检查：`gradlew.bat testDebugUnitTest lintDebug assembleDebug`；release 构建另按签名配置执行。当前实际验证结果单独记录于 `verification.md`，不可把该验收计划当作全部通过声明。

## 9. 小程序的后续准入条件

只有出现明确的免安装或 iOS 需求，再开始独立小程序验证：确认账号及正式分发条件；验证 BLE 权限、MTU 和通知；验证长凭据分包；验证关闭/切后台后的恢复；解决当前固件低 MTU 回执问题；使用同一份协议样例验证行为一致。小程序可先提供只读状态与少量按钮，再决定是否承担敏感凭据配置。

未来目录可使用 `miniprogram/`，本次不创建空工程、不承诺免审核或可取得学校 token。腾讯云的相关 wx API 文档可以辅助理解，但其 Super App 容器文档不等同于微信正式发布规则。

## 10. 来源与未验证事项

本地协议来源与基线见第 2 节；实现时优先使用源码，再更新历史协议文档。以下公开资料查阅日期为 2026-10-26：

- [Android 蓝牙权限](https://developer.android.com/develop/connectivity/bluetooth/bt-permissions)：按系统/目标版本申请权限。
- [Android BLE 数据交换](https://developer.android.com/develop/connectivity/bluetooth/ble/transfer-ble-data)：服务发现、特征访问和通知订阅。
- [BluetoothGatt API](https://developer.android.com/reference/android/bluetooth/BluetoothGatt)：MTU、异步操作与回调约束。
- [微信写特征 API](https://developers.weixin.qq.com/miniprogram/dev/api/device/bluetooth-ble/wx.writeBLECharacteristicValue.html)：官方目标页面本次未能读取，实施小程序前必须重新核验，不将其作为已读证据。
- [腾讯云 BLE Central 文档](https://intl.cloud.tencent.com/ind/document/product/1219/57729)：相关 wx API 的串行写入、报文长度与机型差异说明；文档针对其 Super App 产品，仅作参考。

未验证：目标手机覆盖率、实际协商 MTU、token 最大报文与设备处理耗时、旧固件兼容、正式分发要求、BLE 加密策略。当前结论来自代码审阅与平台文档，未连接开发板、未操作真实账户。

本文件属于仓库实施计划，按 Agent Operating Guide 留在仓库；Notion 本次检索未找到 MoonCheck 既有页面，规范 Documents 目的地查询也无结果。为遵守“不保存可从仓库恢复的信息”，不在 Notion 复制该文档或源码。
