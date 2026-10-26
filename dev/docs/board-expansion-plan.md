# ESP32 多板型扩展实施方案

日期：2026-10-27。状态：首期六目标已实施；修正版联调已获用户确认（具体板型未说明），新增四目标均未经实际验证。C3 SuperMini/C6 保留后续评估。具体交付及反馈要求见[板型支持](../../docs/board-support.md)。保留现有两种构建环境、发行文件名与 v0.6.0 资产。

## 1. 结论与范围

采用“一份业务源码、多个明确硬件配置、按配置生成独立固件”的方式扩展。Android APK 和 Python 控制台继续共用 BLE 协议 1，不按芯片拆分应用。

支持单位是确定的开发板/模组内存规格，而非笼统的芯片名称。同为 ESP32-S3，Flash、PSRAM 类型、LED、USB 接线不同，可能需要不同配置；“SuperMini”等商品名不足以确定引脚和硬件版本。

建议首期候选如下。新环境名均为提案，实施前以已安装平台的 board manifest 和实际硬件核对。

| 候选目标 | 提议环境 | Flash / PSRAM | 优先级 |
| --- | --- | --- | --- |
| 现有 WROOM-32 | `esp32-wroom-32`，保留 | 4 MB / 无 | 必须回归 |
| 现有 S3-N16R8 | `esp32-s3-devkitc-1`，保留 | 16 MB / 8 MB OPI | 必须回归 |
| ESP32-WROOM-32E / 32UE 开发板 | 优先归入现有 WROOM 配置；仅有硬件差异才新增 | 4 MB / 无 | P1，避免重复产物 |
| ESP32-WROVER，确定为 4 MB Flash + 8 MB PSRAM 的板卡 | `esp32-wrover-4mb` | 4 MB / 8 MB，核实模组规格 | P1 |
| ESP32-S3-N8，无 PSRAM 的 DevKitC 类板卡 | `esp32-s3-n8` | 8 MB / 无 | P1 |
| ESP32-S3-N8R8，确定为 OPI PSRAM 的板卡 | `esp32-s3-n8r8` | 8 MB / 8 MB OPI | P1 |
| ESP32-C3-DevKitM-1 | `esp32-c3-devkitm-1` | 4 MB / 无 | P1，资源与单核验收重点 |
| C3 SuperMini | `esp32-c3-supermini-4mb` | 4 MB / 无，核实具体实物 | P2；未知 LED 默认关闭 |
| ESP32-C6 | 暂不固定环境 | 依板卡 | P2，先完成独立工具链试验 |

暂不纳入 S2（缺少 BLE）、H2（缺少原生 Wi-Fi）、P4（需要外置无线协处理器）。C2/C5/C61 等后续按内存、框架和 NimBLE 支持单独评估。不能仅凭支持 BLE 就承诺可运行本项目。

## 2. 当前代码需要解决的问题

1. `firmware/include/app_types.h` 的 `kNetTaskCore=1`，`main.cpp` 使用 `xTaskCreatePinnedToCore`。单核目标改为核心 0，双核保留核心 1；通过 `CONFIG_FREERTOS_UNICORE` / 平台核心数宏做编译期选择，不按芯片名称散布判断。同时检查任务创建返回值，失败应记录明确错误并停止接受需要该任务的操作。
2. `app_types.h` 把 S3 和 C3 的 LED 都默认设成 GPIO48。C3-DevKitM-1 官方 RGB LED 在 GPIO8；应由板型配置指定。补充单色 LED 高/低电平有效参数，未知板卡默认禁用 LED；检查启动绑带脚、Flash/PSRAM 占用脚及 USB 引脚冲突。
3. `dev/tools/make_release.ps1` 的目标数组、`-Only` 筛选和说明文字目前按两板型编写；新增板型必须同时扩展生成、筛选、刷写说明与清单。
4. `tools/check_project.py` 虽按 manifest.boards 遍历，但仍硬编码 app 偏移、NVS 空洞及包内文件。需要改成按板型元数据与实际分区表校验。
5. 当前网络任务栈为 20 KB，日志环形缓冲优先 PSRAM 后回退内部堆，HTTP 正文上限为 32 KB。C3 等无 PSRAM 目标需测量 BLE + Wi-Fi + TLS + 微信加密同时运行时的连续可分配堆和栈余量，不能仅以链接成功判断可用。
6. C6 的当前本机 espressif32 板型定义仅声明 `espidf` 框架；现有 Arduino-ESP32 2.0.17 方案不能直接添加一个 env 即完成支持。

## 3. 配置与代码组织

新增 `firmware/include/board_profile.h`，只放硬件常量及编译期能力校验：板型 ID、LED 引脚/类型/极性、任务核心选择。不在这里复制网络、授权、账号或调度业务逻辑。已有 `WANAN_LED_PIN` / `WANAN_LED_IS_RGB` 覆盖方式继续兼容。

新增 `firmware/board-targets.json`，作为发行板型目录，字段至少包括：

```json
{
  "id": "c3-devkitm-1-4mb",
  "env": "esp32-c3-devkitm-1",
  "chip": "esp32c3",
  "flashSize": "4MB",
  "psram": "none",
  "partitionFile": "partitions/partitions_4mb.csv",
  "status": "experimental"
}
```

上例为设计示意，不是当前已启用记录。实际目录还应记录 flash mode/frequency、USB 模式、LED 配置及硬件验证来源。`platformio.ini` 保留可读的环境配置；打包和检查脚本读取板型目录，并与 PlatformIO 展开的配置及生成的烧录参数交叉核对，发现冲突立即失败，避免出现两个互相漂移的事实来源。

- 保留 `[env]` 公共依赖；抽出经典 ESP32、S3 无 PSRAM、S3 OPI PSRAM、C3 的公共配置段，由具体环境继承。
- 4 MB 目标优先复用现有分区布局，逐目标验证 bootloader 与分区偏移；8 MB 目标新增 ASCII 编码 `partitions_8mb.csv`，保留双 OTA 槽与现有 NVS 命名空间。分区变化必须明确告知用户不能保证原位保留配置。
- WROVER 的 QSPI PSRAM 与 S3 的 OPI PSRAM 分别配置，禁止直接复制 `qio_opi` 到经典 ESP32。无 PSRAM 目标不定义 `BOARD_HAS_PSRAM`。
- USB-UART 和原生 USB 板卡分别验证日志及下载方式；不全局启用 `ARDUINO_USB_*`。不按商家板名猜测接线。
- 可在 `hello` / 诊断返回中新增可选 `boardId`、芯片、Flash/PSRAM 信息，保留原字段语义和协议号。Android 可忽略未知字段；只有需要展示时才修改界面和帮助。

## 4. 构建与发行产物

实施后的预期命令示例（当前新增环境尚不存在）：

```powershell
pio run -d firmware -e esp32-wroom-32 -e esp32-s3-devkitc-1 -e esp32-wrover-4mb -e esp32-s3-n8 -e esp32-s3-n8r8 -e esp32-c3-devkitm-1
```

每个环境使用独立 `.pio/build/<env>/`，不复制覆盖另一环境输出。发行维持现有命名方式，例如：

```text
wanan-<version>-c3-devkitm-1-4mb-merged.bin
wanan-<version>-c3-devkitm-1-4mb-full.zip
wanan-<version>-s3-n8-merged.bin
wanan-<version>-s3-n8-full.zip
```

改造打包脚本支持按完整板型 ID 选择，保留 `-Only s3/wroom32` 的旧行为或提供明确迁移说明。默认完整发行仅纳入已批准的板型集合，实验目标须显式选择并标注状态。APK 仍只有一份。

`release-manifest.json` 增加带版本的板型详细记录：环境、芯片、Flash、PSRAM、分区摘要、各烧录文件和偏移、工具链、源码提交、测试状态。保留已有来源、哈希和 Android 校验。新校验器兼容历史 manifest，不修改历史发行包。

由 PlatformIO 生成信息和实际分区表提取烧录参数，再与目标定义核对。不同芯片 bootloader 起始位置不同，禁止统一套用 `0x1000`；整片 merged 文件仍可统一填充并从 `0x0` 写入，但须逐芯片核验结果。full.zip 内 `FLASH.txt` 必须由该目标的同一份元数据生成。

## 5. 验收门槛

| 层级 | 必须完成 | 可宣称状态 |
| --- | --- | --- |
| 配置检查 | 板型 ID/环境唯一、引脚有效、Flash/分区范围、PSRAM 与 USB 参数正确 | 配置候选 |
| 全量构建 | 原有两目标和全部新增目标均通过；app 放得进每个 OTA 槽 | 编译通过 |
| 离线回归 | Python、Android 协议与界面测试；新增板型目录/产物元数据回归 | 自动验证通过 |
| 实机联调 | 冷启动、重启配置保留、LED、BLE 重连、中文分片、4095 字节命令、Wi-Fi/SNTP、TLS、微信加密/取票、通知及多账号 | 对应板型实测通过 |
| 稳定性 | 建议 24 小时含跨日运行，采集最小堆/最大连续块/栈余量；断网恢复、认证失败、BLE/Wi-Fi 共存均无重启或持续内存下降 | 可申请正式支持 |

真实账号打卡、通知或自动任务仅在用户授权的测试窗口进行。没有实物时保留 experimental，不继承 v0.6.0 的用户联调结论。C3 性能不足时先优化并测量，若仍无法满足全功能要求，不静默减少账号、关闭 TLS 校验或削弱授权流程。

新增检查重点：分区越界拒绝、错误 chip/镜像混配拒绝、缺失目标产物拒绝、无效 LED 配置拒绝、未知 manifest 版本拒绝，以及旧两目标打包结果的结构兼容。

## 6. 实施顺序与交付

1. **基础兼容修复**：提取 board profile，修复单核绑定及 C3 LED，检查任务创建失败。回归现有两目标。
2. **首批板型**：增加 WROVER、S3-N8/N8R8、C3-DevKitM-1 环境；编译并记录资源占用。不同时升级现有框架。
3. **统一发行**：板型目录驱动打包、manifest、FLASH.txt 与检查器；新增离线回归，保持旧版本可校验。
4. **实机验证**：按目标单独填测试矩阵，先实验产物后正式支持。同步 `firmware/README.md`、`docs/flashing.md`、`dev/docs/release-guide.md`、Android 帮助和 `AGENTS.md`。
5. **后续 C6 试验**：在隔离构建配置中选择并固定支持 Arduino-C6 的平台/框架组合，审查来源与版本；验证 Arduino 3.x/ESP-IDF 5.x 对 Wi-Fi、NimBLE、mbedTLS、RGB LED、看门狗和 FreeRTOS 的影响。通过后再决定合并工具链或为新芯片保留独立平台配置，不为增加 C6 而直接替换现有稳定工具链。

本次源码版本已更新为 0.7.0，Android versionCode 为 700。正式发行至少保留现有两目标；只纳入明确通过验收的新增板型，既有 v0.6.0 标签及资产不变。

## 7. 核验依据

- 仓库：`firmware/platformio.ini`、`firmware/include/app_types.h`、`firmware/src/main.cpp`、`firmware/src/status_led.cpp`、`dev/tools/make_release.ps1`、`tools/check_project.py`。
- [Espressif 蓝牙芯片能力表](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/bt-architecture/overview.html)：核实 BLE 能力，不能替代本项目框架适配验证。
- [PlatformIO C3-DevKitM-1](https://docs.platformio.org/en/latest/boards/espressif32/esp32-c3-devkitm-1.html)：board ID、4 MB Flash 与支持框架。
- [Espressif C3-DevKitM-1 硬件指南](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32c3/esp32-c3-devkitm-1/index.html)：官方板 RGB LED 为 GPIO8，不适用于所有第三方 C3 板。
- C6 框架限制依据当前安装的 espressif32 7.1.3 `boards/esp32-c6-devkitc-1.json` 的 `frameworks: ["espidf"]` 与本仓库实际 Arduino 2.0.17 配置；不表示 C6 芯片本身不能运行 Arduino。
