# v0.7.0 项目检查（2026-10-27）

## 发行验收

2026-10-27 用户确认修正版“实测可行”并授权发行；未指明本次具体板型、手机或逐项测试环境，不据此声称两个原有目标均逐板验证。新增四种板型仍未经实际验证。 v0.7.0 按该确认发布为正式 Latest，APK 保持 Debug 签名；原 v0.6.0 标签及资产不变。

## 2026-10-27 状态回执修复

用户实测报告 v0.7.0 APK 连接时持续提示回执不完整。已确认固件 `buildStatusJson()` 将运行指标直接序列化到已有前缀的 String；ArduinoJson 7.4.3 会先清空字符串，导致 fw/chip/runtime 外层丢失、net 等字段错位且出现额外闭括号。Android 严格 JSON 解析因而失败，重查必然复现。

修复为先将 runtime 序列化到独立临时字符串，再拼回状态对象。APK 运行代码不变；同版修复固件重新构建，用户确认后纳入 v0.7.0 发行。升级同一板型、相同分区时只刷 full.zip 中的 firmware.bin 到 0x10000，保留配置；不使用清配置的 merged 镜像作为常规升级。

补充 `tools/test_firmware_status.py`：抽取并直接编译执行生产状态函数，使用项目实际 ArduinoJson 与隔离的 ESP/网络夹具。三个案例覆盖未初始化对象、中文及转义 SSID、断网与无 PSRAM；旧代码错误输出被严格解析器拒绝，修复输出字段与结构均通过。该测试使用 host String 适配器（与设备 String writer 同样清空目标）；不是板卡实机测试。生成的生产回执用于 Android 分片与握手回归，弥补此前仅使用客户端模拟数据的缺口。

本次实现多板型、运行状态与开发者日志；发行版本 0.7.0，Android versionCode 700，协议 1。

- 六个固件目标构建通过：S3-N16R8、WROOM-32、WROVER-4MB、S3-N8、S3-N8R8、C3-DevKitM-1；后四种未经实际验证。
- 维护者只有 S3-N16R8 与 WROOM-32 可以实测，用户已确认修正版实测可行，未指明本次具体板型；历史 v0.6.0 确认不自动继承。
- Android `testDebugUnitTest`：42 项通过（协议 13、请求状态 11、运行指标与日志隐私 5、表单 6、界面 7）；`lintDebug` 无问题，Debug APK 与设备端测试 APK 构建通过。设备端测试 APK 仅编译，未连接手机执行。
- Python 工具测试 14 项、桌面离线核心 23 项全部通过，桌面 AES 交叉验证使用可选 pycryptodome，无跳过。
- 六目标实际构建元数据、镜像 chip ID 与分区二进制校验通过；完整本地包 17 个资产（含 SHA 清单）逐项摘要、ZIP 内容、OTA 槽与 NVS 空洞检查通过。
- 新页面使用 Robolectric 虚构设备状态渲染并进行界面检查；不是实机或真实账号证据。发行包绑定提交后的干净工作区，清单 `sourceDirty=false`；发布流程核对本地源码、标签、远端提交与资产摘要。
- APK 包名 `io.mooncheck.app`、versionName 0.7.0、versionCode 700，v2 签名验证通过。
- 新产物按板型分开，带真实构建元数据与 schema 2 发行清单；实验目标必须显式选择，默认包保持原有两目标。
- [板型支持与反馈模板](board-support.md)列出硬件规格、限制及社区反馈渠道。

本次固件 app 大小（字节，仅为构建证据，不代表运行时内存或硬件验证）：

| 目标 | app 字节 |
| --- | ---: |
| S3-N16R8 | 1,388,464 |
| WROOM-32 | 1,505,616 |
| WROVER-4MB | 1,535,280 |
| S3-N8 | 1,384,608 |
| S3-N8R8 | 1,388,400 |
| C3-DevKitM-1 | 1,473,488 |

以下保留历史记录。

# v0.6.0 项目体检与发行记录

检查日期：2026-10-26。范围：固件两种板型、Android 控制端、PC BLE 控制台、桌面离线核心、用户/开发文档和发行流程。当前发布包括固件及 Android Debug 测试 APK，不重新发行桌面程序。

## 用户实际联调确认与 Latest 发布

2026-10-26，用户明确反馈：“APK实际联调确认通过，功能全部正常”，并要求发布为 Latest。该反馈作为 v0.6.0 功能联调验收依据；原 APK 与固件二进制保持不变，取消 GitHub prerelease 标记并设为 Latest。具体手机型号、Android 版本、板型及逐项测试日志未提供，不推断已经覆盖全部设备兼容矩阵。下方自动检查证据保留为先前构建验证记录。

## 修正的问题

| 问题 | 处理 |
| --- | --- |
| 固件接收缓冲虽为 4096 字节，入队时完整 JSON 被截成 511 字节 | 完整命令使用有界堆缓冲传递所有权，失败与执行后均释放；hello 声明 maxCommandBytes=4095 |
| PC 按每片 UTF-8 解码，中文字符跨片时损坏；长指令一次写入 | 先拼原始字节再解码；按 MTU 字节切片、带响应串行写入；单个换行和 4096 字节总上限 |
| PC raw 命令拆词后重组，连续空格被改变 | 直接解析原始 JSON 子串，保留 SSID/密码空格 |
| PC 只隐藏微信字段；固件 Debug RX 可输出整条配置 | 扩大嵌套字段隐藏范围；RX 只记命令长度；坏回执不直接打印原文 |
| Android 保存成功后的状态查询超时，界面仍可继续操作 | 保存成功提示保留，但断开会话并要求重新查询；不重放已确认的写操作，新增回归测试 |
| 当前文档误称 v0.5.1 尚未发布、Arduino 版本误读、调度说明过时 | 核实远端发行与框架源码，更新为 v0.6.0；运行库实际为 Arduino-ESP32 2.0.17；调度为窗口起点预取票、5 分钟后体检，抖动是 0–jitter 秒延迟 |
| 旧打包会回退到其他工程、旧产物检查较弱、标签上传有歧义 | 只消费本仓库产物，严格版本/时间戳/签名检查；绑定当前提交与目标 origin，先上传草稿并核验摘要再公开 |

## 自动检查证据

| 检查 | 实际结果 |
| --- | --- |
| Android `testDebugUnitTest` | 32 项通过：12 协议、8 请求/状态、6 表单、6 本地界面测试 |
| Android `lintDebug` | `No issues found.` |
| Android `assembleDebug` / `assembleDebugAndroidTest` | 主 APK 与设备测试 APK 均构建成功；未在真实手机执行设备端测试 |
| Python `tools/tests` | 9 项通过：命令、脱敏、中文跨片、长命令、连续空格、超限及坏片 |
| Python `desktop/tests` | 23 项通过，包含 pycryptodome AES 交叉验证；没有跳过项 |
| ESP32-S3 构建 | PlatformIO `esp32-s3-devkitc-1` 成功 |
| ESP32-WROOM-32 构建 | PlatformIO `esp32-wroom-32` 成功；app 约 73.6% 分区占用 |
| 本地静态检查 | Python 源码语法、VERSION/固件/Android 一致性、Markdown 本地文件链接及禁止提交的本机文件类型检查 |
| 包验证 | `tools/check_project.py --release-dir release/v0.6.0` 检查资产摘要、清单提交、ZIP 精确内容、固件 app 偏移及 NVS FF 间隙 |
| APK 包检查 | aapt 核对包名/版本、Android 8+ / targetSdk 35，apksigner 核验签名；升级用 versionCode 600 |
| 升级签名身份 | v0.6.0 APK 与此前本机 0.1.0 APK 的签名证书 SHA-256 一致；未实际安装覆盖验证 |
| 发行保护反例 | 错误的 -Version 被拒绝；带 sourceDirty 的预检清单被 --require-clean 拒绝 |

自动测试共 **64 项通过，0 失败、0 跳过**。构建与打包不包含真实授权凭据，测试不连接学校服务、不提交打卡或真实通知。Android 本地截图使用 Robolectric 与虚构设备状态，不代表真实蓝牙连通；图见 [Android 验证记录](../android/docs/verification.md)。

## 发行内容与溯源

- 固件与 Android 版本统一 `0.6.0`，BLE 协议仍为 1。桌面 1.0.0 仍保留为历史源码，尚不支持微信票据。
- GitHub 发行资产包括两种板型 merged/full、`MoonCheck-0.6.0-android-debug.apk`、`PACKAGE.md`、`release-manifest.json`、整套 ZIP 与 `sha256.txt`。
- 源码提交和每个文件的实际摘要由最终干净提交后的打包清单记录，避免在本文抄写会随重打包变化的校验值。
- 脚本默认只打包；显式 `-Upload` 时核对远端 main 与 HEAD 一致，推送注释标签，创建草稿，校验 GitHub 资产名称/大小/摘要后公开。上传失败保留草稿，不能视为已完成发行。
- v0.6.0 初次以预发布发行；用户确认联调通过后转为正式 Latest，APK 保持原 Debug 签名。[发行页](https://github.com/richard297779/UESTC-MoonCheck/releases/tag/v0.6.0)。二进制源码提交保留原值；文档更新提交单独记入发行清单 documentationCommit。

## 验证边界与后续验收

原自动检查是代码审阅、构建、离线测试及产物检查；其后用户已确认 APK 功能实际联调全部正常。以下内容没有详细证据，或仍属于独立验收范围：

- 不同厂商/Android 版本与两类 ESP32 的真实 BLE、MTU、权限撤销、连接中断与安装升级矩阵。
- 设备上长命令堆分配故障注入、下行交错分片与长时间运行；微信黄金向量自检虽已编译进固件，本次未上板运行。
- 实际功能联调按用户确认通过记录；未提供凭据刷新、取票、登录、提交等逐项原始日志，不补写测试环境或平台响应。平台后续变化仍需重新核验。
- BLE 当前没有显式配对加密或应用鉴权，NVS 凭据及调试日志仍需按实际威胁模型持续审查。请在本人受控的近距离环境使用。
- 正式 Android 发布签名与密钥长期管理、应用市场发布、设备端 UI 测试。当前 Debug 密钥只用于开发，保存在被忽略的本机目录，不随源码或发行包上传。

固件同分区升级只刷 full.zip 中的 firmware.bin 到 0x10000；merged.bin 从 0x0 刷入会清空配置。Android APK 不能刷入开发板，也不能替代开发板独立打卡。操作见[快速开始](quickstart.md)、[刷写说明](flashing.md)与[发行维护指南](../dev/docs/release-guide.md)。
