# v0.8.0 板型支持与验证反馈

更新日期：2026-10-29。固件 / Android 为 **0.8.0**、versionCode **800**、协议 **1**。用户确认“基本功能已查验无误”并授权 Latest；未提供逐板型、手机和逐项流程明细，不补造测试记录。

维护者只能实测 **ESP32-S3-N16R8** 和 **ESP-WROOM-32**。其余四型号均 **未经实际验证**；编译通过仅说明当前工具链可生成产物。欢迎验证者向开发者反馈。

| 发行板型 ID | PlatformIO 环境 | 硬件规格 | 状态 |
| --- | --- | --- | --- |
| `s3-n16r8` | `esp32-s3-devkitc-1` | 16 MB Flash / 8 MB OPI PSRAM；GPIO48 RGB | 原有目标；本次具体测试板型未指明 |
| `wroom32-4mb` | `esp32-wroom-32` | 4 MB Flash，无 PSRAM；GPIO2 普通 LED | 原有目标；本次具体测试板型未指明 |
| `wrover-4mb` | `esp32-wrover-4mb` | 确定为 4 MB Flash / 8 MB QSPI PSRAM；LED 默认关闭 | 实验构建，未经实际验证 |
| `s3-n8` | `esp32-s3-n8` | 8 MB Flash，无 PSRAM；GPIO48 RGB | 实验构建，未经实际验证 |
| `s3-n8r8` | `esp32-s3-n8r8` | 8 MB Flash / 8 MB OPI PSRAM；GPIO48 RGB | 实验构建，未经实际验证 |
| `c3-devkitm-1-4mb` | `esp32-c3-devkitm-1` | 官方 C3-DevKitM-1，4 MB Flash，无 PSRAM；GPIO8 RGB | 实验构建，未经实际验证 |

WROOM-32E/32UE 若 Flash、引脚及电路匹配，可使用 WROOM 配置，但不同模组/板卡仍需反馈验证。S3 板卡修订可能使用不同 LED 引脚；先核对原理图，用 `WANAN_LED_PIN` 等标志覆盖，未知板卡可设 `-1` 关闭。自定义覆盖用于本地调试；正式打包应建立匹配的独立板型记录，校验器不允许用改过硬件参数的构建冒充标准配置。不能把 N16R8 镜像用于 N8/N8R8，也不能把 WROVER PSRAM 配置用于 WROOM。

暂不提供 C3 SuperMini 通用镜像，避免不同商家板型、LED 极性和 USB 接线差异；C6 需要独立框架适配。S2、H2、P4 无法直接满足本项目同时依赖原生 Wi-Fi 与 BLE 的条件。

## 构建与打包

从仓库根目录执行：

```powershell
pio run -d firmware -e esp32-s3-devkitc-1 -e esp32-wroom-32 -e esp32-wrover-4mb -e esp32-s3-n8 -e esp32-s3-n8r8 -e esp32-c3-devkitm-1
python tools/board_targets.py --validate-builds --include-experimental
./android/build-local.ps1
./dev/tools/make_release.ps1 -NoUpload -IncludeExperimental
python tools/check_project.py --release-dir release/v0.8.0
```

默认打包不带 `-IncludeExperimental` 时，只选原有 S3-N16R8/WROOM-32。`-Only c3-devkitm-1-4mb` 可单独选择实验目标；旧 `-Only s3` / `wroom32` 仍只选择原有对应目标。

每个目标独立输出 `wanan-0.8.0-<板型ID>-merged.bin` 与 `…-full.zip`。APK 通用一份，不按板型拆分。目录 `firmware/board-targets.json` 与构建生成 `board-build.json` 交叉核对硬件配置；打包时检查实际镜像芯片、分区范围、OTA 槽容量和烧录偏移。

首次安装 merged 镜像从 `0x0` 刷入，会清空设备配置。full.zip 内含分开的镜像及本目标的 `FLASH.txt`；同分区升级可只刷 app，但分区变化、其他固件迁移均须重新核对。不要混用其他板型的 bootloader 或分区表。

## 新版实测清单

先在 S3-N16R8 / WROOM-32 完成：

1. 安装同签名 APK 更新，连接并确认 boardId、版本、网络名及 IP 正确；断网时网络显示未连接。
2. 内部堆/PSRAM 与串口或设备状态对应；执行正常联网操作后刷新，最低可用与峰值反映分配器高水位；重启后重新统计。PSRAM 为零时显示无可用 PSRAM。
3. 开启开发者日志，读取最近 30 行；敏感行隐藏，关闭选项、离开页面或断开后手机显示清空；该操作不删除设备日志。
4. 保持原有配置、中文长命令、账号槽位、微信取票和通知流程；真实提交与自动任务在本人授权的窗口内验证。
5. 建议持续运行至少 24 小时并覆盖跨日、断网恢复及 BLE 重连，记录最低堆、最大连续块和网络任务栈最低余量。

## 向开发者提供验证信息

欢迎通过 [GitHub Issues](https://github.com/richard297779/UESTC-MoonCheck/issues) 反馈。请注明“社区验证”，维护者核验前不自动升级为官方实测。

```text
固件版本 / 构建提交：
板型 ID / 模组丝印 / 开发板名称与硬件修订：
Flash 大小 / PSRAM 大小及类型：
供电与下载方式（USB-UART / 原生 USB）：
手机型号 / Android 版本 / APK 版本：
冷启动、BLE 配网/重连、Wi-Fi/SNTP、授权取票、通知结果：
运行时长 / 重启情况：
最低可用堆 / 最大连续块 / 栈最低余量：
问题复现步骤与脱敏日志：
```

不要发送账号、密码、Cookie、浏览器指纹、微信凭据、推送密钥。网络名/IP/宿舍位置等也应自行遮盖。只有编译证据时填写“编译通过、未实测”，不要填“全部正常”。
