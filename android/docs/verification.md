# Android 实现与验证记录

> 本文保留 Android 0.1.0 初始开发记录及当时 APK 的摘要。v0.6.0 发行检查、更新后的测试数与资产以[项目体检](../../docs/project-health.md)和发行包清单为准。

验证日期：2026-10-26。应用版本：0.1.0，versionCode 1，包名 `io.mooncheck.app`。

## 实际交付

- Kotlin / Jetpack Compose 工程与 Gradle Wrapper。
- 四个主要入口：控制台、配置、账号、帮助；支持系统深浅色主题。
- 设备扫描与人工选板、服务发现、MTU 校验、通知订阅、握手、串行写入和按 ID 匹配业务回执。
- WiFi、单人账号、浏览器指纹、微信授权、坐标、时间、自动开关的表单控制。
- 多人账号增删启停、槽位显示、授权导入、演练与间隔设置。
- 默认及附加通知配置、通道状态与启停、关联修改、测试与删除。
- 体检、网络检查、微信取票与链路测试、最近活动、重置确认。
- 12 篇可离线搜索的内置帮助，包含首次使用、连接、授权、坐标、多人、通知、超时及隐私说明。
- 开发测试 APK：仓库根目录 `release/android/MoonCheck-0.1.0-debug.apk`，17,466,449 字节。

APK SHA-256：

```text
7bb4cc94b470e0d44871c1fcb404748ac0b54a32e5b94db54aff25a0a8e6c1e7
```

APK 是 Debug 签名版本；未创建正式发行密钥、未发布到应用市场，未执行设备刷写。

## 已执行的验证

通过 `android/build-local.ps1` 完成以下任务：

| 检查 | 结果 |
| --- | --- |
| `testDebugUnitTest` | 31 项全部通过，0 失败 |
| 协议编解码测试 | 12 项：UTF-8 跨片、base64、长度边界、缺片/重复/冲突、旧字符串片与未知字段等 |
| 请求与状态测试 | 7 项：握手、账号槽位、超时不重放、ID 隔离、部分写入失败、并发阻止、长命令能力兼容 |
| 表单测试 | 6 项：空密码网络、空格、坐标、时间、通知关闭、YES 确认、槽位与到期时间 |
| 本地界面测试 | 6 项：离线首页、已配置示例、配置校验、账号说明、帮助搜索、真实提交确认 |
| `lintDebug` | `No issues found.` |
| `assembleDebug` | 生成 APK |
| `assembleDebugAndroidTest` | 生成设备端 UI 测试 APK；仅编译，未在设备运行 |
| `apksigner verify --verbose` | APK 签名校验通过，v2 签名 |
| `aapt dump badging` | minSdk 26、targetSdk 35、版本及包名与工程一致 |
| `git diff --check` | 通过 |

本机构建环境无法正常启动软件模拟器；没有更改 Windows 虚拟化设置，也没有操作已有用户模拟器。改用 Robolectric Android 35 原生图形模式直接绘制 Android View，生成生产 Compose 界面的截图，并逐张检查首页、配置、账号与帮助的中文显示、间距、导航和可滚动布局。截图包含虚构状态，**不代表已连接真实 BLE 设备**。

- [未连接首页](screenshots/01-dashboard-offline.png)
- [已配置示例首页](screenshots/02-dashboard-demo.png)
- [配置向导](screenshots/03-configuration.png)
- [账号页](screenshots/04-accounts.png)
- [离线帮助](screenshots/05-help.png)

## 为长凭据修复的固件兼容问题

代码审阅确认，原 `BleService` 接收缓冲为 4096 字节，但 `CommandRouter::WorkItem.payload` 只有 512 字节，`enqueue` 会静默截断较长的 JSON。BLE 分包本身不能绕过该限制。

本次最小修复：普通任务继续使用定长 payload；完整 BLE 命令改为最多 4095 字节的独立堆缓冲，由队列传递所有权，入队失败自动释放，执行后自动释放；`hello` 新增 `maxCommandBytes:4095`。不把整个 8 项队列固定扩成大缓冲，避免所有任务持续额外占用大量 RAM。

Android 读取该能力；没有字段的旧固件按 511 字节上限，在发送前阻止过长配置。旧固件的基本短命令仍可使用。若微信授权超出旧上限，请按固件 README 编译并刷入本次修复后的固件。没有自动刷写或 OTA。

实际执行 `platformio run -e esp32-s3-devkitc-1 -e esp32-wroom-32`，两种目标均编译成功；未做刷板、堆分配故障注入或真实长凭据导入测试。

## 尚未完成的发布验收

- 未用实际安卓手机连接任一 ESP32，MTU、权限撤销、跨厂商稳定性与后台恢复需要真机验证。
- 未在设备运行 `connectedDebugAndroidTest`，本地 JVM 界面测试不替代设备端验证。
- 未执行真实账号登录、授权导入、通知发送或打卡提交。
- 未完成 minSdk 26 至不同 Android 版本的真机兼容矩阵，深色主题和大字体仍需人工设备验收。
- 未完成正式签名、安装升级验证、应用市场分发与发布前 BLE 配对/加密或设备鉴权改造。当前固件的链路保护限制仍存在。
- 长报文下行外壳没有消息组 ID；交错组冲突会按失败处理，未修改固件的整组发送互斥策略。

结论：当前是完成实现、构建及本地验证的 **开发测试版**，适合进入真实手机与开发板联调，不能将其描述为已经过完整生产验收的正式发行版。
