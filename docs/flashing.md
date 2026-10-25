# 刷固件

> 如果已经有支持当前平台要求的固件文件，不需要安装 PlatformIO 或编译；用 Chrome / Edge 的网页烧录工具即可，全程约 3 分钟。
>
> **当前版本注意：** GitHub Releases 中已发布的 v0.4.0 预编译固件不支持平台新增的微信操作票据要求。当前源码版本为 v0.5.1；在 v0.5.1 Release 发布前，请先按[自行编译](../firmware/README.md)构建固件。建议直接使用 PlatformIO 的 Upload；若使用网页工具，本页的地址和刷写方式适用于源码构建产物及发行包。

---

## 1. 先确认板子型号

看芯片上的丝印或开发板标签：

| 芯片 | 对应板型 | 选哪个固件 |
| --- | --- | --- |
| 写 **ESP32-S3**（常见 S3-N16R8，16 MB Flash） | S3 | `wanan-<版本>-s3-n16r8-merged.bin` |
| 写 **ESP32**（经典 WROOM-32，4 MB Flash） | WROOM-32 | `wanan-<版本>-wroom32-4mb-merged.bin` |

在 [Releases](https://github.com/richard297779/UESTC-MoonCheck/releases) 页面下载对应文件。

> ⚠️ 刷错型号会表现为：串口日志 `invalid header magic`，或者刷完蓝牙根本不广播。

---

## 2. 两种文件，别选错

| 文件 | 用途 | 烧录地址 | 配置 |
| --- | --- | --- | --- |
| `…-merged.bin` | **首次刷机 / 恢复出厂** | `0x0` | **会被清空**（WiFi、账号、坐标、通知全部重置） |
| `…-full.zip` 里的 `firmware.bin` | **只升级固件** | `0x10000` | 完整保留 |

`merged.bin` 是出厂整片镜像，段间空洞被填成 `0xFF`，其中包括存放配置的 NVS 分区（`0x9000`），所以刷完等于恢复出厂。

只想升级版本？用 `firmware.bin`，不要把 merged 包刷上去。

---

## 3. 网页刷写（推荐）

1. 用 **Chrome 或 Edge** 打开 <https://espressif.github.io/esptool-js/>
   （Firefox / Safari 不支持 Web Serial，不能用。）

2. 用 **USB 数据线**把板子接到电脑。注意有些线只能充电、不能传数据。

3. 波特率先选 `921600`（连不上再降到 `115200`），点 **Connect**，在弹窗里选串口。

4. 如果连不上，让板子进入下载模式：
   按住 **BOOT** 不放 → 点一下 **RESET / EN** → 松开 BOOT → 再点 Connect。

5. 文件表里**只填一行**：

   | 地址 | 文件 |
   | --- | --- |
   | `0x0` | 刚下载的 `…-merged.bin` |

   Flash Mode / Frequency / Size 保持 `keep` / `keep` / `detect`——固件头里已经带好正确参数。

6. 点 **Program**，等进度条走完，出现 `Hash of data verified` 即成功。

7. 点 **Disconnect** 释放串口，再按一下板子的 **RST** 复位。

看到下面这行是**正常成功**，不是错误（只是串口被占用没法自动复位）：

```text
Hard resetting via RTS pin...
```

---

## 4. 升级固件（保留配置）

1. 下载新版本的 `…-full.zip`，解压出 `firmware.bin`。
2. 网页工具里仍然先 Connect。
3. 文件表里**只填一行**：地址 `0x10000`，文件 `firmware.bin`。
4. Program → 完成后 Disconnect + RST。

WiFi、账号表、坐标、通知地址全部保留，不需要重新配置。

---

## 5. 换分区表 / 救砖（完整四文件）

只有在**换分区方案**（例如 4 MB 板改用双槽布局）或整片刷写失败时才需要。

| 地址（S3） | 地址（WROOM） | 文件 |
| --- | --- | --- |
| `0x0` | `0x1000` | `bootloader.bin` |
| `0x8000` | `0x8000` | `partitions.bin` |
| `0xe000` | `0xe000` | `boot_app0.bin` |
| `0x10000` | `0x10000` | `firmware.bin` |

文件都在 `…-full.zip` 里。四个文件一起填，一次 Program。

> 换分区表后 NVS 布局会变，**配置需要重新写一遍**（`0x9000` 起的分区不会被这四行覆盖，但旧配置可能不再匹配）。

---

## 6. 报错速查

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 页面里没有串口可选 | 驱动没装 / 线只能充电 | 装 CH340 或 CP210x 驱动，换数据线 |
| `No serial port selected` | 没先 Connect | 先点 Connect 再 Program |
| 一直停在 `Connecting....` | 没进下载模式 | 按住 BOOT → 点 RESET → 松开 → 重连 |
| `Invalid head of packet (0x08)` | 波特率太高 / USB 口不稳 | 降到 115200，换机箱后面的 USB 口 |
| `could not claim interface` | 串口被其他程序占用 | 关掉串口助手 / 另一个浏览器标签 / Arduino IDE |
| `invalid header magic` | 刷错型号 | 换对应板型的 bin |
| 刷完蓝牙搜不到 | 刷错型号，或没按 RST | 确认型号，按 RST，重新上电 |
| 配置全没了 | 刷了 merged 包 | 重新做 [快速开始](quickstart.md) 的配置 |
| 卡在 `Hard resetting via RTS pin...` | 串口被占用，未自动复位 | 已成功；Disconnect 后手动按 RST |
| 反复重启 / `brownout` | 供电不足 | 换 5 V ≥ 1 A 电源，换短线 |

## 7. 命令行兜底（可选）

装了 Python 和 esptool 的话：

```powershell
python -m pip install esptool

# S3：整片刷
python -m esptool --chip esp32s3 -p COM5 -b 921600 write_flash 0x0 wanan-0.3.1-s3-n16r8-merged.bin

# WROOM-32：整片刷
python -m esptool --chip esp32 -p COM5 -b 921600 write_flash 0x0 wanan-0.3.1-wroom32-4mb-merged.bin

# 只升级固件
python -m esptool --chip esp32s3 -p COM5 -b 921600 write_flash 0x10000 firmware.bin

# 看串口日志（排查用）
python -m esptool --chip esp32s3 -p COM5 -b 115200 run
```

`COM5` 换成设备管理器里实际的端口号。

---

## 8. 刷完之后

网页烧录**不负责配置**。还需要通过蓝牙把 WiFi、账号、坐标、打卡时间写进设备：

→ 继续看 **[快速开始](quickstart.md)** 第 4 步（或用安卓手机 + nRF Connect，见 [指令手册](commands.md#用-nrf-connect-配置)）。

配置完成后，用 `chain 0` 和 `status` 验收即可。
