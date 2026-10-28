# 电脑端独立发行补充

本轮发布 `desktop-v1.1.0` 预发行版，包含桌面 Windows 程序和电脑 BLE GUI 源码 ZIP。根目录 VERSION / 固件 / APK 仍为 0.7.0，不重新上传、换标或改动既有 v0.7.0 资产；固件 Latest 保持 v0.7.0。电脑端未完成真实账号/GUI BLE 联调，因此使用 Prerelease。

构建和核验流程：

```powershell
./desktop/build-local.ps1
python -m unittest discover -s tools/tests -v
python tools/check_project.py
python dev/tools/check_privacy.py --source
# 提交所有本次源码、文档后，重新运行 package_local.py 记录干净 HEAD：
python desktop/package_local.py
python dev/tools/package_computer_release.py
# 在已安装 PyInstaller 的构建环境执行，包含冻结 Python 模块检查：
python dev/tools/check_privacy.py --frozen desktop/dist/windows/MoonCheck.exe desktop/dist/release/desktop-v1.1.0/MoonCheck-Desktop-1.1.0-windows.zip desktop/dist/release/desktop-v1.1.0/MoonCheck-BLE-Console-1.1.0-source.zip
```

电脑端包装脚本不上传。产物在 `desktop/dist/release/desktop-v1.1.0/`：两个 ZIP、computer-release-manifest.json、sha256.txt。校验源码提交、ZIP 白名单、内部摘要和构建源文件哈希。Windows EXE 无生产代码签名，Linux 未构建；BLE 工具包为源码，需要自行安装 Python/tkinter/bleak。发布说明及验证边界见[本轮发行说明](computer-release.md)。

发布时先检查 origin、远端 main 和干净 HEAD，使用独立注释标签 desktop-v1.1.0；先建 GitHub 草稿预发行版，上传四资产，核对远端资产名、大小与 SHA-256 后公开，设置 latest=false。不覆盖既有标签或资产。

历史跟踪固件 ZIP 中发现编译路径元数据，未发现完整私钥 PEM；仅有 TLS 解析器内置的 BEGIN 标记。历史二进制保持原始哈希，不纳入本轮资产，也不重写 Git 历史；`--include-historical` 可单独扫描，不能将其表述为已清除历史副本。当前源码与本轮资产单独扫描。

---

以下为固件 / Android 0.7.0 的发行流程与历史记录，命令中的 0.7.0 不适用于电脑端独立标签。

# v0.7.0 发行维护指南

默认发行包含原有两种 ESP32 固件、原生 Android **Debug 签名测试版**、完整发行 ZIP、使用说明、SHA-256 与源码提交清单。桌面 1.1.0 已独立移植微信票据但尚待实测，单独构建，不混入固件发行包。

## 版本与范围

- 仓库根目录 `VERSION` 是项目与 Android versionName 的来源；固件 `FW_VERSION` 必须与它一致。
- 当前 `0.7.0`，Android versionCode 为 `700`，BLE 协议仍为 `1`。
- 历史桌面版本独立维护，不通过改数字假装支持新票据。
- 2026-10-27 用户确认 v0.7.0 修正版实测可行，授权正式发行；本次具体板型未说明，不补造逐板型记录。新增板型全部未经实际验证，详见[支持矩阵](../../docs/board-support.md)。
- `make_release.ps1` **不编译，默认不上传**；没有 `-Upload` 时只生成本地包。

## 1. 构建并验证

```powershell
# 仓库根目录；JDK 17 / Android SDK 35 已准备
./android/build-local.ps1
pio run -d firmware -e esp32-s3-devkitc-1 -e esp32-wroom-32
python -m unittest discover -s tools/tests -v
python -m unittest discover -s desktop/tests -v
python tools/check_project.py
```

状态组包回归（需要 C++17 本机编译器，或 `--zig <路径>`）：

```powershell
python tools/test_firmware_status.py --compiler c++
```

该命令编译生产 `buildStatusJson()`，使用实际 ArduinoJson，重现旧错误并验证修复，随后更新 `android/app/src/test/resources/firmware-status.json`。改动固件组包后先运行它，再执行 Android 测试；只跑客户端模拟响应不足以验证设备 JSON。

桌面 AES 交叉验证需要可选依赖 pycryptodome；缺少时会明确跳过该测试，其余离线测试仍可运行。本次体检在项目本地测试依赖中安装该包并运行了交叉验证。

工具链：PlatformIO espressif32 7.1.3 / NimBLE-Arduino 2.5.1 / ArduinoJson 7.4.3。注意框架包 `4.20017.260907+sha.dcc1105b` **对应 Arduino-ESP32 2.0.17**，不是 4.2.17；以 `esp_arduino_version.h` 为证。Android 使用 Gradle 8.11.1、AGP 8.9.2、Kotlin 2.1.20 和 Compose BOM 2025.04.01。

## 2. 本地打包

`firmware/board-targets.json` 管理六目标；`board-build.json` 记录 PlatformIO 实际芯片、Flash、PSRAM、LED、分区摘要和偏移，打包前必须一致。schemaVersion=2 清单包含 boardProfiles、分区源与验证状态；旧 schema 1 清单继续按旧格式解析。发行包只包含明确选择的目标。

```powershell
# 六种构建（新增四种未经实机验证）
pio run -d firmware -e esp32-s3-devkitc-1 -e esp32-wroom-32 -e esp32-wrover-4mb -e esp32-s3-n8 -e esp32-s3-n8r8 -e esp32-c3-devkitm-1
python tools/board_targets.py --validate-builds --include-experimental
# 默认只选原有两个目标；以下显式包含全部实验目标
./dev/tools/make_release.ps1 -NoUpload -IncludeExperimental
# 也可以 -Only c3-devkitm-1-4mb；旧 -Only s3 / wroom32 保留
```

实验目标的 FLASH.txt / manifest / PACKAGE 须保留未经实际验证声明与反馈入口。本次 v0.7.0 在用户确认后正式发布为 Latest；未来未确认版本可用 -Prerelease。


```powershell
./dev/tools/make_release.ps1 -NoUpload
python tools/check_project.py --release-dir release/v0.7.0
```

脚本只读取本仓库 `firmware/.pio/build/<env>`，不再自动回退到别的工程或优先选择 COM 调试环境。严格检查源码新鲜度、固件版本、APK 内部包名/版本、APK 签名；不允许给旧产物强贴新版本。APK 默认取 `android/app/build/outputs/apk/debug/app-debug.apk`，可用 `-AndroidApk` 指定路径，仍须通过同样检查。

产物统一位于 `release/v<版本号>/`，当前为 `release/v0.7.0/`；历史版本也按同样结构归档，入口见[发行目录索引](../../release/README.md)。打包暂存位于 `release/.staging/v<版本号>/`，不混入发行目录或 ZIP。源码维护的本版说明为 `release/v0.7.0/CHANGELOG.md`，脚本生成的 `notes-0.7.0.md` 是上传用草稿，两者职责不同。

产物位于 `release/v0.7.0/`：

| 文件 | 用途 |
| --- | --- |
| `wanan-0.7.0-s3-n16r8-merged.bin` | S3-N16R8 出厂镜像，刷 `0x0`，会清配置 |
| `wanan-0.7.0-s3-n16r8-full.zip` | S3 四文件包；同分区升级只刷 app 到 `0x10000` |
| `wanan-0.7.0-wroom32-4mb-merged.bin` | WROOM-32 出厂镜像，刷 `0x0`，会清配置 |
| `wanan-0.7.0-wroom32-4mb-full.zip` | WROOM 四文件包；同分区升级只刷 app 到 `0x10000` |
| `MoonCheck-0.7.0-android-debug.apk` | Android 8+，近距离 BLE 控制器测试版 |
| `MoonCheck-0.7.0-release.zip` | 上述固件、APK、PACKAGE.md、release-manifest.json 的整套包 |
| `release-manifest.json` | 版本、源码提交、板型、APK 元信息、各内层资产摘要 |
| `PACKAGE.md` | 包内用户指南 |
| `sha256.txt` | 单独资产与整套 ZIP 的摘要；ZIP 本身通过内置 manifest 校验内部文件 |

临时 staging 路径每次唯一，资产由明确列表打包，历史发行文件不删除、不自动上传、不把本机配置或签名密钥放入 ZIP。`-Only s3/wroom32` 用于单板调试打包；`-SkipAndroid` 显式跳过 APK，完整发行不使用它。旧 `-SkipDesktop` 参数仍可接受，桌面版默认即不包含。

## 3. 提交、推送、绑定标签并发布

先提交源码、文档与截图，再推送 main。构建产物/密钥/本机 SDK 路径不应加入 Git；历史已跟踪的发行记录继续保留。

```powershell
git add <本次源码和文档路径>
git commit -m "release: ship v0.7.0 with Android BLE controller"
git push origin main
./dev/tools/make_release.ps1 -Upload -IncludeExperimental -ValidationNote "2026-10-27 用户确认修正版实测可行；具体板型未说明，新增四目标未经实测" -Repo richard297779/UESTC-MoonCheck
```

上传流程检查工作区干净、远端 main 与本地 HEAD 相同、origin 与指定 Repo 一致，然后创建注释标签并推送。`gh release create` 使用 `--verify-tag --target <HEAD>`；先创建草稿，逐个核对 GitHub 返回的文件名、大小及 SHA-256 后再公开。任何上传不完整都保留草稿，不宣称发布完成。不覆盖旧标签或旧版本资产。

若网络中断导致草稿已经存在，请先检查其标签和实际资产，再恢复缺失上传并重新逐项核验；不要移动已有标签或盲目 clobber。连接器不提供 Release 上传时，使用已登录的 GitHub CLI；不要把访问令牌写入脚本。

## 4. 使用者刷写与升级

| 项目 | S3-N16R8 | WROOM-32 |
| --- | --- | --- |
| bootloader | `0x0` | `0x1000` |
| partitions | `0x8000` | `0x8000` |
| boot_app0 | `0xe000` | `0xe000` |
| firmware | `0x10000` | `0x10000` |
| Flash | 16 MB / dio / 80 MHz | 4 MB / dio / 40 MHz |

两种 merged 镜像都从 `0x0` 刷入，但不能互换。合并镜像在 NVS 空洞填 FF，会清空 WiFi、账号、坐标、计划、授权等配置。同分区升级仅刷 app；分区表变化或从其他固件迁移需另行核对布局，不保证保留配置。

用户入口见 [刷固件](../../docs/flashing.md)、[快速开始](../../docs/quickstart.md) 与 [Android 说明](../../android/README.md)。
