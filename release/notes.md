# 发行说明

## v0.3.1

- 修复 Server酱通知正文为空（正文键为 `desp`）
- 修复经典 ESP32-WROOM-32 在部分板型上打卡异常
- 单一固件：打卡人数由账号表决定（`account add` / `account on|off`，最多 6 人）

---

## 下载哪个文件

先看芯片丝印：

| 芯片 | 选择 |
| --- | --- |
| **ESP32-S3**（N16R8，16 MB） | `wanan-0.3.1-s3-n16r8-merged.bin` |
| **ESP32**（WROOM-32，4 MB） | `wanan-0.3.1-wroom32-4mb-merged.bin` |

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
- 从源码编译见 **[自行编译](../firmware/README.md)**

---

## 免责

本固件仅供你在**自己或已知情同意**的同学账号上使用。
请自行确认符合学校相关规定；因使用产生的账号风险由使用者自担。MIT License。
