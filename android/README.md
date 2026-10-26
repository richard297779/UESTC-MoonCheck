# MoonCheck Android 控制端

v0.7.0 原生 Kotlin + Jetpack Compose 安卓控制端，通过 BLE 连接 ESP32。包含控制台、配置向导、账号管理、诊断、通知管理与可离线搜索的帮助。

## 使用

1. 安装 Debug APK 后打开应用，点击“连接设备”。首次使用授予附近设备权限，旧安卓按提示开启定位权限与系统定位开关。
2. 核对名称和 MAC 后手动选择自己的板子，等待握手完成。
3. 在配置页逐项保存 WiFi、本人账号、微信授权、宿舍坐标与时间计划。每项以设备回执确认成功。
4. 执行设备体检、微信取票测试，再主动开启每日自动任务。配置完成后手机无需常驻。
5. 帮助页可以离线搜索连接、授权、多人槽位与使用说明。

**兼容性注意：** 目标为 Android 8+、固件协议 1。旧固件虽然 BLE 缓冲为 4096 字节，但队列会把 JSON 截到 511 字节。v0.6.0 已在固件源码修复，并由 `hello.maxCommandBytes` 声明 4095 字节 JSON 上限。APK 对没有该能力字段的旧固件按 511 字节限制，在表单中拒绝过长配置。较长微信凭据请先编译并刷入修复后的固件。

从 [v0.7.0 发行页](https://github.com/richard297779/UESTC-MoonCheck/releases/tag/v0.7.0) 下载 `MoonCheck-0.7.0-android-debug.apk` 或整套 ZIP。2026-10-27 用户确认状态回执修复后的版本实测可行并授权发行；APK 保持 Debug 签名。未提供逐板型、手机和系统明细，不推断覆盖完整兼容矩阵。

## v0.7.0 运行状态与开发者选项

控制台 → **运行状态** → 刷新，可查看设备实际连接的网络名、IP、信号、板型、运行时长、内部堆当前/最低可用、分配器使用峰值、最大连续块、PSRAM 与任务栈最低余量。指标单位为 KiB，统计自本次开机，重启重置；内部堆与 PSRAM 分开显示。

开启“开发者选项 / 查看设备日志”，手动读取最近 30 行。敏感行整体隐藏，日志不持久化、不导出；关闭选项、退出该页或断开连接会清空手机显示，不删除设备环形日志。日志仍可能含环境信息，请勿未经审查公开。

旧协议 1 固件仍能连接，缺失的新指标显示“固件未提供”。只有 S3-N16R8 与 WROOM-32 可由维护者实测，本版修复联调已获用户确认（具体板型未说明）；新增型号均未经实机验证，欢迎按[反馈要求](../docs/board-support.md)提交信息。本次 v0.7.0 确认独立于历史 v0.6.0，未扩大到全部板型。

## 构建

工具链固定为 JDK 17、Gradle 8.11.1、AGP 8.9.2、Kotlin 2.1.20、Compose BOM 2025.04.01、Android SDK 35，minSdk 26。

使用 Android Studio 打开本目录，安装 SDK Platform 35 与 Build Tools 35.0.0。`local.properties` 填入本机 `sdk.dir`，或配置 `ANDROID_HOME`。Wrapper 校验官方分发包 SHA-256；依赖版本见 `gradle/libs.versions.toml`。

Windows：

```powershell
# 在 android/ 目录执行；先配置 JAVA_HOME 为 JDK 17
./build-local.ps1
```

也可直接执行（Gradle 从仓库根目录 `VERSION` 读取 versionName；versionCode 当前为 700）：

```text
gradlew.bat testDebugUnitTest lintDebug assembleDebug assembleDebugAndroidTest
```

`build-local.ps1` 会把 Gradle 缓存与 Java 临时文件放在项目内，避免某些 Windows 环境的短路径临时目录不可写。Gradle 自动在被忽略的 `.gradle/` 中生成专用 Debug 签名，不使用正式发布密钥；换签名后需先卸载旧测试版才能安装。

- APK：`app/build/outputs/apk/debug/app-debug.apk`
- 单元及本地界面测试报告：`app/build/reports/tests/testDebugUnitTest/index.html`
- lint 报告：`app/build/reports/lint-results-debug.html`
- Robolectric 渲染图：`app/build/reports/screenshots/`（虚构测试状态）
- 设备端测试：连接专用测试手机或模拟器后执行 `gradlew.bat connectedDebugAndroidTest`。不会主动连接真实开发板或提交打卡。

本机下载的工具只位于被忽略的 `.build-tools/`，不作为项目依赖提交。正式发布需自行配置独立签名、验证 APK 安装升级，并保管签名密钥。

## 结构与维护

`ble/` 管理 Android 蓝牙回调与串行操作，`protocol/` 负责 UTF-8 字节分片与回执重组，`data/` 负责请求与设备状态，`ui/` 提供 Compose 页面、表单校验和内置帮助。所有敏感输入仅保留于当前表单；离开应用断开连接，未完成操作不会自动重放。

- [方案、协议约束与实施步骤](docs/implementation-guide.md)
- [实际验证结果、截图与未完成验收](docs/verification.md)
- 协议事实来源：[Python 控制台](../tools/ble_console.py)、[固件 BLE 服务](../firmware/src/ble_service.cpp)、[命令路由](../firmware/src/command_router.cpp)
- 用户操作参考：[指令手册](../docs/commands.md)、[快速开始](../docs/quickstart.md)

微信小程序仍作为后续备选。本工程不在手机执行学校登录或独立打卡，也不自动读取微信、Cookie 或浏览器授权。
