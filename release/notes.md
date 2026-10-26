# 发行说明

## v0.6.0（Android 控制端与固件兼容更新）

- 项目与 Android 版本统一为 0.6.0，Android versionCode 600；BLE 协议仍为 1。
- 新增原生 Android BLE 控制端：整洁卡片界面、配置向导、按键控制、多人授权槽位、通知管理、诊断和离线搜索帮助。2026-10-26 用户确认 APK 实际联调通过、功能全部正常，现为 Latest 正式发行；APK 仍为同一 Debug 签名构建。
- 固件修复完整命令被任务队列截到 511 字节的问题；`hello.maxCommandBytes` 声明 JSON 上限 4095 字节。RX 日志改为只记录长度，避免打印完整配置。
- PC 控制台修复中文跨片乱码、长报文字节切片、raw JSON 连续空格保留及敏感字段隐藏，增加坏分片保护。
- Android 在配置已保存、后续状态查询超时时断开连接并保留“已确认保存”的提示，避免静默恢复到可操作状态。
- 打包流程默认只生成本地包；检查版本和产物新鲜度，增加固件 + APK 整套 ZIP、源码提交清单和 SHA-256；上传绑定明确提交并逐资产校验后发布。
- 修正文档中过时的“v0.5.1 未发行”、框架版本与调度抖动说明；实际 Arduino-ESP32 运行库为 2.0.17。
- 本版不重新发行旧桌面二进制。完整验证范围与未实测事项见[项目体检](../docs/project-health.md)。

## v0.5.1

- 延续设备端微信票据链，改进凭据刷新与失败分类；已发行两种板型的预编译固件。
- 不含本次新增 Android 控制端与长命令队列修复。

## v0.5.0

- 平台自 2026-10-23 起要求打卡请求携带 `X-Checkin-Ticket`。当前固件源码已加入微信小程序授权链，在设备端自动换取并附加短效操作票据。
- 新增 `wx.import`、`wx.status`、`wx.test` BLE 指令；授权凭据按账号顺序存入槽位，并在刷新后持久化。
- 本次只发行 ESP32 固件，不附带桌面版可执行文件。
- 初始微信凭据需每个账号本人通过 PC 端 QR Connect 扫码授权，并从 DevTools Cookie 中读取；扫码地址、步骤与 `expires_in` 换算说明见[快速开始](../docs/quickstart.md#22-准备微信授权凭据)。
- 发布资产：`wanan-0.5.0-s3-n16r8-merged.bin`、`wanan-0.5.0-s3-n16r8-full.zip`、
  `wanan-0.5.0-wroom32-4mb-merged.bin`、`wanan-0.5.0-wroom32-4mb-full.zip`。

## v0.4.0

**新功能**

- **多通道通知**：默认通道之外最多再挂 5 条附加通道，每条可关联若干账号——
  每个人的打卡结果单独推给每个人（`notify add/link/on/off/rm`，详见 [手机通知](../docs/notify.md)）。
- 多人轮的推送改为「逐账号结果 → 关联通道，整轮汇总 → 默认通道」，去重与配额按通道独立。

**本版本附带的可执行文件**

| 文件 | 给谁用 | 怎么用 |
| --- | --- | --- |
| `MoonCheck-1.0.0-windows-x64.exe` | Windows 10/11（无需开发板） | 双击 = 图形界面；带参数 = CLI。首次运行在 exe 同目录生成 `config.json` |
| `MoonCheck-1.0.0-linux-x86_64` | x86-64 Linux（无需开发板） | `chmod +x` 后直接运行，用法同上 |
| `wanan-0.4.0-s3-n16r8-merged.bin` | ESP32-S3 板 | 刷 `0x0`（整片重置，会清配置） |
| `wanan-0.4.0-wroom32-4mb-merged.bin` | ESP32-WROOM-32 板 | 刷 `0x0`（同上） |

桌面版（多账号、打卡时间窗、按人推送与固件同语义）使用说明见 **[桌面版](../desktop/README.md)**；
固件版刷写与配置见 **[刷固件](../docs/flashing.md)** 与 **[快速开始](../docs/quickstart.md)**。
只想升级旧固件、不想丢配置：用对应 `-full.zip` 里的 `firmware.bin` 刷 `0x10000`。

---

## v0.3.1

- 修复 Server酱通知正文为空（正文键为 `desp`）
- 修复经典 ESP32-WROOM-32 在部分板型上打卡异常
- 单一固件：打卡人数由账号表决定（`account add` / `account on|off`，最多 6 人）

---

## 下载哪个文件（固件）

先看芯片丝印：

| 芯片 | 选择 |
| --- | --- |
| **ESP32-S3**（N16R8，16 MB） | `wanan-0.6.0-s3-n16r8-merged.bin` |
| **ESP32**（WROOM-32，4 MB） | `wanan-0.6.0-wroom32-4mb-merged.bin` |

只想升级、不想丢配置？下载对应的 `-full.zip`，只刷里面的 `firmware.bin` 到 `0x10000`。

---

## 刷入

1. 用 **Chrome 或 Edge** 打开 <https://espressif.github.io/esptool-js/>
2. USB 数据线接板子，波特率 `921600`（连不上改 `115200`），点 **Connect** 选串口
3. 地址填 `0x0`，选合并包，点 **Program**
4. 完成后 **Disconnect** + 按 **RST**

> 完整步骤、升级方式、报错处理见 **[刷固件](../docs/flashing.md)**。

⚠️ `-merged.bin` 是出厂镜像，会**清空 WiFi、账号、坐标、通知**等全部配置。

---

## 刷完之后

网页烧录不负责配置。还需要通过蓝牙把 WiFi、账号、坐标、打卡时间写入设备：

- 电脑：**[快速开始](../docs/quickstart.md)**
- 安卓手机 + nRF Connect：**[指令手册](../docs/commands.md#用-nrf-connect-配置)**

配置完用 `chain 0` 和 `status` 验收。

---

## 校验与构建

- 文件校验值见同目录 `sha256.txt`
- 构建环境：PlatformIO Core 6.2.0 / platform espressif32 7.1.3 / Arduino-ESP32 4.2.17 /
  esptool 4.11.0 / NimBLE-Arduino 2.5.1 / ArduinoJson 7.4.3
- 桌面版构建：PyInstaller 单文件（Windows/Linux 各自平台构建；运行时零第三方依赖，
  打包环境 conda `mooncheck-desktop`，Python 3.12 + PyInstaller 6.22）
- 从源码编译见 **[自行编译](../firmware/README.md)**；桌面版从源码构建见 **[桌面版](../desktop/README.md)** 第 5 节

---

## 免责

本固件仅供你在**自己或已知情同意**的同学账号上使用。
请自行确认符合学校相关规定；因使用产生的账号风险由使用者自担。MIT License。
