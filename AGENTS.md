# MoonCheck Agent Guide

适用于整个仓库。当前用户请求优先；修改前阅读相关源码和模块文档。以下命令默认从仓库根目录执行，除非另有说明。本文依据 2026-10-27 的仓库扫描编写，工具链版本以对应配置文件为准。

## 技术栈与目录

| 目录 | 技术栈与职责 |
| --- | --- |
| `firmware/` | C++17、PlatformIO、Arduino-ESP32 2.0.17、FreeRTOS；ESP32 网络、调度、账号、BLE 与通知 |
| `android/` | Kotlin 2.1.20、Jetpack Compose Material 3、协程、kotlinx.serialization；原生 BLE 控制端 |
| `tools/` | Python 用户工具；`ble_console.py` 使用 bleak，`check_project.py` 执行仓库与发行包检查 |
| `desktop/` | Python 3.10+、tkinter、标准库 HTTP、CAS AES、PyCryptodome；独立版本 1.1.0 的微信取票桌面程序 |
| `docs/` | 用户指南 |
| `dev/docs/`、`dev/tools/` | 协议、开发设计、验证方案与开发辅助工具 |
| `release/` | 按 `vX.Y.Z/` 保存发行产物，根目录 README 为索引；`.staging/` 单独保存打包暂存 |

- 六目标目录见 `firmware/board-targets.json`；新增 WROVER-4MB、S3-N8/N8R8、C3-DevKitM-1 均未经实机验证，默认发行仍只选原有两个目标。固件原有环境为 `esp32-s3-devkitc-1`（S3-N16R8）与 `esp32-wroom-32`（4 MB、无 PSRAM）。账号数量由运行配置决定，不创建单人/多人构建变体。
- 固件依赖固定在 `firmware/platformio.ini`：espressif32 7.1.3、NimBLE-Arduino 2.5.1、ArduinoJson 7.4.3。PlatformIO 框架包版本不等于 Arduino 运行库版本。
- Android 使用 JDK 17、Gradle 8.11.1、AGP 8.9.2、Compose BOM 2025.04.01、SDK 35、minSdk 26。依赖集中在 `android/gradle/libs.versions.toml`。
- 桌面 1.1.0 已移植微信授权/MMTLS/取票，需按 desktop/requirements.txt 安装依赖；未完成真实桌面账号验证，不沿用固件实测结论。桌面版本由 desktop/VERSION 独立维护，默认不混入固件发行。

## 开发环境

- 用 `git status --short` 检查已有修改；保留用户修改。搜索优先使用 `rg`、`git ls-files`，避开 `.pio/`、`.gradle-home/`、`.build-tools/` 和 `build/` 缓存。
- 安装 Python 3.10+ 与 PlatformIO CLI；运行 BLE 控制台需要 `python -m pip install bleak`。桌面微信链路运行依赖 PyCryptodome。
- Android Studio 打开 `android/`；安装 SDK Platform 35、Build Tools 35.0.0，配置 `JAVA_HOME` 以及 `android/local.properties` 的 `sdk.dir` 或 `ANDROID_HOME`。
- Windows 优先用 `android/build-local.ps1`；它管理项目内缓存及临时目录，并恢复调用前环境。`.build-tools/` 中的本机工具是可选缓存，不是已提交的依赖。
- 不把某位开发者的绝对 SDK、Python、JDK 路径写入源码。

## 构建与测试命令

### 仓库及 Python 检查

```powershell
python tools/check_project.py
python -m unittest discover -s tools/tests -v
python -m unittest discover -s desktop/tests -v
```

`check_project.py` 检查版本一致性、Python 语法、仓库文档链接和已跟踪文件中的敏感路径。桌面 AES 交叉验证依赖可选 `pycryptodome`；可在隔离的测试环境执行 `python -m pip install pycryptodome`，缺少依赖时须报告测试跳过。

### 固件

改动状态组包时，运行 `python tools/test_firmware_status.py --compiler c++`（需要本机 C++17 编译器及已安装的项目 ArduinoJson）；也可用 `--zig <便携zig路径>`。它直接编译生产函数并生成 Android 回执测试夹具，不访问设备或账号。再执行 Android 测试验证分片/握手。ArduinoJson 7 的 serializeJson 会清空 String，禁止将它用于向非空 String 追加嵌套 JSON。

```powershell
pio run -d firmware -e esp32-s3-devkitc-1 -e esp32-wroom-32
```

涉及共用代码、BLE、内存或分区的修改需构建板型目录中的全部六目标，并运行 `python tools/board_targets.py --validate-builds --include-experimental`。编译不等于硬件验证；刷写、清空 NVS、联网账号操作按当前任务授权执行，并核对板型与端口。

### Android

```powershell
./android/build-local.ps1
```

默认执行 `testDebugUnitTest`、`lintDebug`、`assembleDebug`、`assembleDebugAndroidTest`。`-SkipTests` 仅用于快速构建，不可作为完整验证。

直接使用 Wrapper 时先进入 `android/`：

```powershell
./gradlew.bat testDebugUnitTest lintDebug assembleDebug assembleDebugAndroidTest
# 有专用测试手机或模拟器时，执行设备端测试：
./gradlew.bat connectedDebugAndroidTest
```

Linux/macOS 使用 `./gradlew`。生成测试 APK 不代表设备端测试已执行。

- APK：`android/app/build/outputs/apk/debug/app-debug.apk`
- 单元测试报告：`android/app/build/reports/tests/testDebugUnitTest/index.html`
- lint 报告：`android/app/build/reports/lint-results-debug.html`
- Robolectric 渲染图：`android/app/build/reports/screenshots/`；这些虚构状态截图不作为真实 BLE 联调证据。

### 桌面打包与发行包检查

- 桌面改动先安装 desktop/requirements-build.txt，执行 desktop/tests；Windows 可用 `./desktop/build-local.ps1` 测试并构建。
- Windows 桌面打包：在 `desktop/` 执行 `./build_windows.bat`，需要 Miniconda。
- Linux 桌面打包：在 Linux/WSL 的 `desktop/` 执行 `bash build_linux.sh`；PyInstaller 不跨平台打包。
- 完整发行流程见 `dev/docs/release-guide.md`。以下脚本只打包已有构建，默认不上传：

```powershell
./dev/tools/make_release.ps1 -NoUpload
python tools/check_project.py --release-dir release/v<版本号>
```

把 `<版本号>` 替换为根目录 `VERSION` 的实际值。正式核验可追加 `--require-clean`，但要求工作区干净、清单来源与当前提交一致；历史发行包不能直接当成新提交产物。

## 工程约定

- 根目录 `VERSION` 是项目版本与 Android `versionName` 的来源；固件 `FW_VERSION` 与其保持一致，Android `versionCode` 随新版本递增。桌面由 desktop/VERSION 独立维护，并同步 __version__ 与 Windows version_info.txt。
- 保持现有模块边界：Android 的 `ble/`、`protocol/`、`data/`、`ui/` 分别处理蓝牙、帧协议、请求状态与界面；界面不直接拼接底层 BLE 操作。
- BLE 当前协议为 Nordic UART Service 上的协议 1，命令使用 UTF-8 JSON 加换行。事实来源为 `firmware/src/ble_service.cpp`、`firmware/src/command_router.cpp` 和 `tools/ble_console.py`；设计文档中的历史设想不能视为已实现功能。
- 保持 MTU 分片、串行写入、回执确认、超时与断连语义；base64 分片先合并字节再解码 UTF-8，避免中文跨片损坏。
- 新固件通过 `hello.maxCommandBytes` 声明 JSON 上限 4095 字节（加换行 4096）；无该字段的旧固件按 511 字节兼容。修改时同步固件、Python、Android 的协议与边界校验。
- 运行状态的内部堆峰值由设备分配器最低可用值计算，开机重置；PSRAM 单列。开发者日志须脱敏、限量、不持久化，离开页面/断线清空；不将历史联调结论沿用于新版本或新增型号。
- BLE 回调不执行阻塞网络操作；保持任务队列缓冲的所有权和失败释放逻辑，禁止重新引入命令截断。
- Android 表单以设备回执确认保存成功；敏感输入保持现有临时生命周期，未完成操作不自动重放。新增用户功能同步离线帮助。
- 用户行为改变时同步 `docs/` 与模块 README；协议、架构、发行过程改变时同步 `dev/docs/`，区分当前实现和历史记录。
- 遵循现有代码风格与 `.gitattributes` 的 LF 文本规则；二进制保持 binary，不做无关批量格式化。
- 根据行为变化补充有意义的回归测试；按影响范围执行上述检查。仅文档修改通常运行链接/一致性检查即可。明确报告失败、跳过、未执行项目及真实设备验证的来源和范围。

## 提交与 PR

- 提交前检查 `git diff --check`、实际 diff 与暂存路径，只提交本次相关文件；提交信息沿用 `docs:`、`fix:`、`release:` 等简洁前缀。
- PR 描述说明问题、最终行为、相关验证及限制；有仓库模板时优先使用模板。不要套用本项目不存在的 pnpm/Turbo 命令或项目名格式。
- 发布遵循发行指南：核对版本、构建新鲜度、来源提交、APK 内部版本/签名、SHA-256、远端资产；完成校验后才宣称发布成功。

## 禁止事项

- 不提交密码、Cookie、浏览器指纹、微信凭据、票据、通知密钥、真实账号配置、HAR/抓包、签名私钥或本机配置；日志、截图、测试数据及错误输出同样需要脱敏。
- 发行文件不散放在 `release/` 根目录；各版本的资产和校验清单保持同目录，解压副本放 `vX.Y.Z/extracted/`。历史已跟踪资产移动后保留追踪资格，不因此把新构建输出加入 Git。
- 不提交 `.pio/`、Gradle 缓存、SDK、构建输出、APK 或本地密钥，不为提交产物随意放宽 `.gitignore`；保留历史已跟踪记录。
- 不以修改版本字符串掩盖旧产物，不移动已发布标签，不盲目覆盖发行资产，不混入其他项目构建。
- 不把 Debug 签名 APK 宣称为正式生产签名；保留现有本地签名以支持升级，不擅自更换密钥。
- 不混刷 S3 与 WROOM 镜像，不把 merged 全量刷写描述为保留配置。全量镜像会清空 NVS；分区兼容时仅 app 升级另行核对。
- 未经任务授权，不主动连接真实板卡、提交真实打卡、发送测试通知或开启每日调度；自动测试使用虚构数据和隔离环境。
- 不在未获当前任务授权时推送、发布或创建定时任务；不执行破坏性 Git 重置、强推或丢弃用户修改。

## Notion 与持久工作空间

- 使用 Notion 作为用户的持久共享记忆和工作空间，当前请求始终优先。
- 每次会话首次处理依赖个人/项目背景的工作或写入 Notion 前，读取 用户私人 Agent Operating Guide v0.0.4（位置由会话指令提供，不在公开仓库记录私人页面 ID）（发布于 2026-10-13）；会话开始获取已启用的 Notion Skills，仅在当前授权范围内使用。
- 回答依赖个人或项目背景的问题前搜索 Notion；无结果时检查规范记忆目的地。保留来源、日期和不确定性，优先更新已有内容。
- 保存持久决策、偏好、纠正和工作成果；不保存秘密、凭据、闲聊、临时进度或可从仓库恢复的代码。
- 文档、研究与计划优先保存在现有私人 Documents 数据库；代码、README、仓库计划及本工程指南留在仓库，不重复归档可从仓库恢复的内容。
- 新增结构、首次归档到共享空间、更改共享、删除、归档、移动、重组或安排定时工作前先获得批准。
- 编辑前重读，写入后核验；遵守排除项和遗忘请求。Notion 不可用时明确说明未保存，仅报告实际完成的动作。

电脑端独立预发行使用 `desktop-v<desktop/VERSION>` 标签，按 `dev/docs/release-guide.md` 顶部流程打包；无需为纯电脑端变更重建或换标固件/APK。`dev/tools/check_privacy.py --source` 检查当前源码，历史固件资产仅以 `--include-historical` 单独审计且保持原始哈希。
