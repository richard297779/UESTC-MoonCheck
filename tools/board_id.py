#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""多板场景的"串口 ↔ 板子"对照表：读每块板的 MAC / 芯片型号 / Flash 容量。

为什么需要：好几块板同时插着时，Windows 只会给你一串 COM 号（S3 原生 USB 的 VID:PID
全都一样），根本分不清哪个是哪个；而 `pio run -t upload` 只有在你**指定端口**时才不会刷错。

用法：
    python tools/board_id.py                  # 自动枚举串口（需 pyserial）
    python tools/board_id.py --ports COM5,COM7
    python tools/board_id.py --json           # 机器可读输出，供脚本选端口

⚠️ 会短暂复位被探测的板子（esptool 会拉 DTR/RTS 进下载模式再放回）。
   正在无人值守跑打卡的板子，别在 21:00–23:30 之间探测。
"""
import argparse
import json
import os
import re
import subprocess
import sys

MAC_RE = re.compile(r"MAC:\s*([0-9a-fA-F:]{17})")
CHIP_RE = re.compile(r"(?:Chip is|Detecting chip type\.\.\.)\s*([A-Za-z0-9\-]+)")
FLASH_RE = re.compile(r"Detected flash size:\s*([0-9]+)\s*MB", re.I)

ENV_HINT = {
    "ESP32": "esp-wroom-32",
    "ESP32-S3": "esp32-s3-devkitc-1",
    "ESP32S3": "esp32-s3-devkitc-1",
    "ESP32-C3": "esp32-c3（需把 net_task 绑核改为 core 0）",
}


def find_esptool() -> str:
    """优先用 PlatformIO 自带的 esptool，避免额外安装。"""
    candidates = [
        os.path.expanduser(r"~\.platformio\packages\tool-esptoolpy\esptool.py"),
        os.path.expanduser(r"~\.platformio\packages\tool-esptoolpy\esptool"),
    ]
    for item in candidates:
        if os.path.exists(item):
            return item
    return ""


def list_ports() -> list:
    try:
        from serial.tools import list_ports
    except ImportError:
        print("缺少 pyserial（自动枚举串口需要）：python -m pip install pyserial\n"
              "或者手动指定：python tools/board_id.py --ports COM5,COM7")
        return []
    return [item.device for item in sorted(list_ports.comports(), key=lambda x: x.device)]


def probe(esptool: str, port: str) -> dict:
    info = {"port": port, "chip": "", "mac": "", "flashMB": 0, "error": ""}
    for command in ("read_mac", "flash_id"):
        try:
            proc = subprocess.run(
                [sys.executable, esptool, "--chip", "auto", "--port", port,
                 "--baud", "115200", command],
                capture_output=True, text=True, timeout=25, encoding="utf-8", errors="replace")
            output = (proc.stdout or "") + (proc.stderr or "")
        except subprocess.TimeoutExpired:
            info["error"] = "超时（可能被占用/未进下载模式）"
            return info
        except Exception as exc:  # noqa: BLE001
            info["error"] = str(exc)
            return info

        match = MAC_RE.search(output)
        if match:
            info["mac"] = match.group(1).lower()
        match = CHIP_RE.search(output)
        if match:
            info["chip"] = match.group(1).upper()
        match = FLASH_RE.search(output)
        if match:
            info["flashMB"] = int(match.group(1))
        if "Unable to connect" in output or "Connecting.." in output and not info["mac"]:
            info["error"] = info["error"] or "无响应（试按 BOOT+RST 进下载模式）"
    if info["mac"]:
        octets = [int(item, 16) for item in info["mac"].split(":")]
        # esptool 读到的是 WiFi/基础 MAC；蓝牙 MAC = 基础 MAC + 1（固件广播名取蓝牙 MAC 末两字节）
        wifi_tail = "%02X%02X" % (octets[4], octets[5])
        bt_tail = "%02X%02X" % (octets[4], (octets[5] + 1) & 0xFF)
        info["bleName"] = "WANAN-%s" % bt_tail
        info["bleNameAlt"] = "WANAN-%s" % wifi_tail
        info["env"] = ENV_HINT.get(info["chip"], "（未知芯片，请核对 board）")
    return info


def main() -> int:
    parser = argparse.ArgumentParser(description="串口 ↔ 板子（MAC/芯片/Flash）对照表")
    parser.add_argument("--ports", help="逗号分隔，如 COM5,COM7；不给则自动枚举")
    parser.add_argument("--json", action="store_true", help="输出 JSON")
    args = parser.parse_args()

    esptool = find_esptool()
    if not esptool:
        print("找不到 PlatformIO 的 esptool.py（~/.platformio/packages/tool-esptoolpy/）")
        return 2

    ports = ([item.strip() for item in args.ports.split(",") if item.strip()]
             if args.ports else list_ports())
    if not ports:
        print("没有可探测的串口。")
        return 1

    results = [probe(esptool, port) for port in ports]

    if args.json:
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 0

    print("\n串口 ↔ 板子 对照表")
    print("  端口    芯片          Flash   BLE 广播名     MAC                  建议 env")
    print("  " + "-" * 92)
    for item in results:
        if not item["mac"]:
            print("  %-7s %-12s %5s  %-14s %-20s %s"
                  % (item["port"], item["chip"] or "?", "-", "-", "-",
                     item["error"] or "未识别"))
            continue
        print("  %-7s %-12s %4sM  %-14s %-20s %s"
              % (item["port"], item["chip"], item["flashMB"], item["bleName"], item["mac"],
                 item["env"]))
    print("\n烧录指定板子：  pio run -e <env> -t upload --upload-port <端口>")
    print("BLE 指定板子：  python tools/ble_console.py --name <广播名>")
    return 0


if __name__ == "__main__":
    sys.exit(main())
