#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""晚安成电终端 · PC 端蓝牙调试台

用途：通过 BLE 给 ESP32 配网与执行自测指令，避免在手机上手工敲 JSON。

依赖：
    python -m pip install bleak

用法：
    python tools/ble_console.py                      # 只有一台时自动连接；多台会列出让选
    python tools/ble_console.py --list               # 列出所有可见设备（名字/RSSI/MAC）
    python tools/ble_console.py --name WANAN-1A2B    # 按广播名连接（推荐多板使用）
    python tools/ble_console.py --index 0            # 按 --list 的序号连接
    python tools/ble_console.py --addr AA:BB:CC:DD:EE:FF
    python tools/ble_console.py hello status probe   # 依次执行若干命令后退出

多块板子怎么区分？
    · 广播名 WANAN-XXXX 的后缀 = 该板 MAC 的最后两字节（出厂即唯一）；
    · 连上后 `hello` 会回报**完整 MAC + Flash/PSRAM 容量**（不同容量=不同型号）；
    · 想知道哪个 COM 口对应哪块板：python tools/board_id.py（读 MAC/芯片/Flash）。

交互命令（也适用于命令行）：
    hello                      握手
    status                     配置与运行状态
    time                       设备时间 / 是否在打卡窗口内
    logs [n]                   拉取最近 n 行日志
    wifi <ssid> [pass]         配置 WiFi（会立即连接）；开放网络省略 pass
    scan                       扫描周边 AP（设备只能看到 2.4 GHz：channel 1–13）
    netcheck                   校园网连通性体检：请求 generate_204
                               （204=已放行；302/200+HTML=被 Portal 劫持，需网页认证）
    auth <user> <pass> [bfp]   配置学号 / 密码 / 浏览器指纹
    loc <lat> <lng> [addr]     配置打卡坐标
    sched <at> [end] [jitter]  配置打卡时间，如 sched 22:00 23:30 30
    auto <on|off>              开关每日自动打卡
    account list               列出多人账号（学号打码，不含密码/指纹）
    account add <标签> <学号> <密码> [指纹|-] [lat lng [地址…]]
                               新增或覆盖一个账号（最多 6 个；省略坐标则沿用设备当前坐标，
                               指纹填 - 表示不设置）
    account rm <标签>          删除一个账号；account clear 清空全部
    multi on|off [gap_s]       开关"多人顺序打卡"（gap_s = 账号之间间隔秒，默认 20）
    multi [probe] [force]      立即按顺序跑一轮。probe=只体检不提交（**仍会登录**，完整演练）；
                               force=跳过"已打卡"检查；窗口外演练需 `multi probe force` 才允许登录
    name [标签]                查看/设置 BLE 广播名（如 name A01 → WANAN-A01；
                               不带参数则显示当前名字；标签只允许 A-Za-z0-9-_ ≤12 字符）
    http <url>                 访问任意 URL（链路自测）
    login [force]              登录链路自测（force = 打卡窗口外也强制登录）
    chain [wait_s] [force]     全链路自测：登录页→登录→进入微应用→业务接口
                               （wait_s = 等待平台放行的秒数，最长 900，默认 0）
    probe                      前置体检（只读，随时可用）
    checkin [force]            打卡（force=跳过"今日已打卡"检查）
    clear                      清空 Cookie（模拟会话失效）
    session <value>            注入外部 SESSION（调试用，例如浏览器窗口内建立的会话）
    reset                      恢复出厂（会二次确认）
    raw <json>                 直接发送原始 JSON
    quit / exit                退出
"""
import argparse
import asyncio
import base64
import json
import sys
import time

try:
    from bleak import BleakClient, BleakScanner
except ImportError:  # pragma: no cover
    print("缺少依赖 bleak，请先执行：python -m pip install bleak")
    sys.exit(2)

NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # 设备 -> 手机（Notify）
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # 手机 -> 设备（Write）

HELP_TEXT = __doc__.split("交互命令", 1)[1] if "交互命令" in __doc__ else ""


class Console:
    """把交互命令翻译成 BLE JSON 指令并打印回执。"""

    def __init__(self, client: BleakClient):
        self.client = client
        self.next_id = 1
        self.pending_chunks = {}
        self.received = asyncio.Queue()
        self._loop = asyncio.get_event_loop()

    # ---------------------------------------------------------------- BLE

    def _on_notify(self, _sender, data: bytearray) -> None:
        text = data.decode("utf-8", "replace").strip()
        if not text:
            return
        try:
            message = json.loads(text)
        except json.JSONDecodeError:
            print(f"\n<= {text}")
            return

        # 长数据分片重组：b64 = base64 分片（新），chunk = 转义分片（兼容旧固件）
        if "seq" in message and "of" in message and ("b64" in message or "chunk" in message):
            total = int(message["of"])
            seq = int(message["seq"])
            if seq == 0:
                self.pending_chunks.clear()  # 新的一组分片开始，丢弃上一组残留
            if "b64" in message:
                piece = base64.b64decode(message["b64"]).decode("utf-8", "replace")
            else:
                piece = message["chunk"]
            buffer = self.pending_chunks.setdefault(total, {})
            buffer[seq] = piece
            if len(buffer) == total:
                full = "".join(buffer[i] for i in range(total))
                self.pending_chunks.pop(total, None)
                self._print_json(full)
            return
        self._print_json(text)

    @staticmethod
    def _print_json(text: str) -> None:
        try:
            pretty = json.dumps(json.loads(text), ensure_ascii=False, indent=2)
        except json.JSONDecodeError:
            pretty = text
        print(f"\n<= {pretty}")

    async def send(self, payload: dict) -> None:
        payload.setdefault("id", self.next_id)
        self.next_id += 1
        line = json.dumps(payload, ensure_ascii=False) + "\n"
        print(f"=> {json.dumps(payload, ensure_ascii=False)}")
        await self.client.write_gatt_char(NUS_RX, line.encode("utf-8"), response=False)
        await asyncio.sleep(0.15)

    # ---------------------------------------------------------------- 命令

    async def run_command(self, line: str) -> bool:
        """执行一条命令；返回 False 表示要退出。"""
        parts = line.strip().split()
        if not parts:
            return True
        command, args = parts[0].lower(), parts[1:]

        if command in ("quit", "exit", "q"):
            return False
        if command in ("help", "?"):
            print(HELP_TEXT)
            return True
        if command == "raw":
            try:
                await self.send(json.loads(" ".join(args)))
            except json.JSONDecodeError as exc:
                print(f"  JSON 解析失败：{exc}")
            return True

        # 简单映射
        simple = {
            "hello": {"op": "hello"},
            "status": {"op": "status"},
            "time": {"op": "test.time"},
            "probe": {"op": "test.probe"},
            "clear": {"op": "session.clear"},
            "netcheck": {"op": "test.http", "url": "http://connect.rom.miui.com/generate_204"},
        }
        if command in simple:
            await self.send(simple[command])
            return True

        if command == "login":
            # 默认遵守固件的窗口策略（窗口外登录不会被平台授权，且白费一次登录次数）；
            # login force 才会在打卡窗口外强制登录，仅用于验证登录链本身。
            payload = {"op": "test.login"}
            if args and args[0] in ("force", "true", "1"):
                payload["force"] = True
            await self.send(payload)
            return True

        if command == "chain":
            # 全链路自测：把"登录成功"与"平台真的放行微应用"分开验证。
            # 平台存在生效延迟（实测登录成功约 10 分钟后才允许进入界面），
            # 因此 wait_s 会每 30 秒重试一次"进入微应用"。
            payload = {"op": "test.chain"}
            for arg in args:
                if arg.lower() in ("force", "true", "1"):
                    payload["force"] = True
                else:
                    try:
                        payload["wait_s"] = max(0, min(900, int(arg)))
                    except ValueError:
                        print(f"  忽略无法识别的参数：{arg}")
            await self.send(payload)
            return True

        if command == "session":
            if not args:
                print("  用法：session <SESSION值>（把外部会话注入设备，用于只读验证）")
            else:
                await self.send({"op": "session.set", "value": args[0]})
        elif command == "logs":
            await self.send({"op": "logs", "n": int(args[0]) if args else 30})
        elif command == "scan":
            # 扫描周边 AP（只看 2.4 GHz，channel 1-13）；等价 raw {"op":"wifi.scan"}
            await self.send({"op": "wifi.scan"})
        elif command == "wifi":
            if not args:
                print("  用法：wifi <ssid> [pass]（开放网络可省略 pass，例如校园网 UESTC-WiFi）")
            else:
                await self.send({"op": "wifi.set", "ssid": args[0],
                                 "pass": args[1] if len(args) >= 2 else ""})
        elif command == "auth":
            if len(args) < 2:
                print("  用法：auth <user> <pass> [bfp]")
            else:
                payload = {"op": "auth.set", "user": args[0], "pass": args[1]}
                if len(args) >= 3:
                    payload["bfp"] = args[2]
                await self.send(payload)
        elif command == "loc":
            if len(args) < 2:
                print("  用法：loc <lat> <lng> [addr]")
            else:
                payload = {"op": "loc.set", "lat": float(args[0]), "lng": float(args[1])}
                if len(args) >= 3:
                    payload["addr"] = " ".join(args[2:])
                await self.send(payload)
        elif command == "sched":
            if not args:
                print("  用法：sched <at> [window_end] [jitter_s]")
            else:
                payload = {"op": "sched.set", "at": args[0]}
                if len(args) >= 2:
                    payload["window_end"] = args[1]
                if len(args) >= 3:
                    payload["jitter_s"] = int(args[2])
                await self.send(payload)
        elif command == "auto":
            await self.send({"op": "auto.set", "enabled": (args[0] != "off") if args else True})
        elif command == "name":
            # 不带参数 → hello（回执含 bleName/btMac/flash）；带参数 → 设置自定义广播名
            if args:
                await self.send({"op": "name.set", "name": args[0]})
            else:
                await self.send({"op": "hello"})
        elif command == "account":
            sub = args[0].lower() if args else "list"
            if sub in ("list", "ls"):
                await self.send({"op": "account.list"})
            elif sub == "clear":
                await self.send({"op": "account.clear"})
            elif sub in ("rm", "remove", "del"):
                if len(args) < 2:
                    print("  用法：account rm <标签>")
                else:
                    await self.send({"op": "account.remove", "label": args[1]})
            elif sub == "add":
                if len(args) < 5:
                    print("  用法：account add <标签> <学号> <密码> [指纹|-] [lat lng [地址…]]")
                    print("        省略坐标 → 沿用设备当前坐标；指纹不需要就填 -")
                else:
                    payload = {"op": "account.add", "label": args[1],
                               "user": args[2], "pass": args[3]}
                    if args[4] != "-":
                        payload["bfp"] = args[4]
                    if len(args) >= 7:
                        try:
                            payload["lat"] = float(args[5])
                            payload["lng"] = float(args[6])
                        except ValueError:
                            print("  经纬度必须是数字")
                            return True
                        if len(args) >= 8:
                            payload["addr"] = " ".join(args[7:])
                    await self.send(payload)
            else:
                print("  用法：account list | add | rm | clear")
        elif command == "multi":
            if args and args[0] in ("on", "off"):
                payload = {"op": "multi.set", "enabled": args[0] == "on"}
                if len(args) >= 2:
                    payload["gap_s"] = int(args[1])
                await self.send(payload)
            else:
                # 可组合：multi / multi force / multi probe / multi probe force
                payload = {"op": "test.multi"}
                words = {word.lower() for word in args}
                if "force" in words:
                    payload["force"] = True
                if "probe" in words:
                    payload["probe"] = True
                await self.send(payload)
        elif command == "http":
            if not args:
                print("  用法：http <url>")
            else:
                await self.send({"op": "test.http", "url": args[0]})
        elif command == "checkin":
            payload = {"op": "test.checkin"}
            if args and args[0] in ("force", "true", "1"):
                payload["force"] = True
            await self.send(payload)
        elif command == "reset":
            answer = input("  确认擦除全部配置与凭据？输入 YES 继续：").strip()
            if answer == "YES":
                await self.send({"op": "factory.reset", "confirm": "YES"})
        else:
            print(f"  未知命令：{command}（输入 help 查看）")
        return True


async def scan_devices(prefix: str, timeout: float = 15.0) -> list:
    """扫描所有匹配的设备，按信号强度降序返回 ``[(name, address, rssi), ...]``。"""
    print(f"扫描 BLE 设备（寻找 {prefix}-* 与自定义名，最长 {timeout:.0f} 秒）...")
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    found = []
    for device, adv in devices.values():
        name = (adv.local_name or device.name or "").strip()
        if not name:
            continue
        if name.upper().startswith(prefix.upper()) or name.upper().startswith("WANAN"):
            found.append((name, device.address, adv.rssi, device))
    found.sort(key=lambda item: -(item[2] if item[2] is not None else -999))
    return found


def print_devices(devices: list) -> None:
    """打印候选设备表（多板场景下必须让用户自己选）。"""
    print(f"\n发现 {len(devices)} 台设备：")
    print("  序号  广播名            RSSI   地址")
    for index, (name, address, rssi, _device) in enumerate(devices):
        print(f"  {index:>4}  {name:<16} {str(rssi):>5}   {address}")
    print("\n  选择方式：--name WANAN-1A2B   /   --index 0   /   --addr AA:BB:CC:DD:EE:FF")
    print("  提示：广播名后缀 = MAC 末两字节；`hello` 指令会回报完整 MAC / Flash / PSRAM，")
    print("        串口侧对应关系用 tools/board_id.py 查（COM 口 ↔ MAC ↔ 芯片型号）。\n")


async def find_device(prefix: str, timeout: float = 15.0):
    """扫描并返回唯一匹配的设备；**多台时不猜，直接列出候选并返回 None**。"""
    devices = await scan_devices(prefix, timeout)
    if not devices:
        return None
    if len(devices) > 1:
        print_devices(devices)
        return None
    name, address, _rssi, device = devices[0]
    print(f"找到 {name} ({address})")
    return (name, address), device


async def resolve_target(args) -> list:
    """按 --addr / --name / --index / 自动扫描 解析出连接目标列表。"""
    if args.addr:
        return [args.addr]

    devices = await scan_devices(args.prefix, args.scan_timeout)
    if not devices:
        print("未找到设备。请确认设备已上电、未被其它手机/程序占用，然后重试。")
        return []

    if args.list:
        print_devices(devices)
        return []

    if args.name:
        wanted = args.name.upper()
        matched = [item for item in devices
                   if item[0].upper() == wanted or item[0].upper().startswith(wanted)]
        if not matched:
            print(f"没有名字匹配 {args.name} 的设备。当前可见：")
            print_devices(devices)
            return []
        if len(matched) > 1:
            print(f"{args.name} 匹配到多台，请用完整名字或 --index：")
            print_devices(matched)
            return []
        print(f"选中 {matched[0][0]} ({matched[0][1]})")
        return [matched[0][3], matched[0][1]]

    if args.index is not None:
        if not 0 <= args.index < len(devices):
            print(f"--index {args.index} 超出范围")
            print_devices(devices)
            return []
        chosen = devices[args.index]
        print(f"选中 {chosen[0]} ({chosen[1]})")
        return [chosen[3], chosen[1]]

    if len(devices) == 1:
        print(f"找到 {devices[0][0]} ({devices[0][1]})")
        # WinRT 下"设备对象"通常比 MAC 字符串更可靠 → 两个目标轮流试
        return [devices[0][3], devices[0][1]]

    # 多台且没指定 → 不猜
    print(f"\n⚠️ 同时在线 {len(devices)} 台设备，**不自动挑一台**（避免连错板子）。")
    print_devices(devices)
    return []


async def connect_with_retry(targets: list, timeout: float, attempts: int = 3) -> BleakClient:
    """按目标列表轮流尝试连接并自动重试。

    Windows(WinRT) 下"读 GATT 服务表"这一步容易失败，表现为三种错误：
        - ``TimeoutError``
        - ``BleakError: Could not get GATT services: Unreachable``
        - ``asyncio.CancelledError``
    绝大多数情况是 **PC 侧**原因：Windows 缓存了旧的 GATT 记录，或设备已被别的中心占用。
    因此这里交替使用「扫描得到的设备对象」和「MAC 地址字符串」两种目标，并拉长重试间隔。
    """
    last_error = None
    for attempt in range(1, attempts + 1):
        target = targets[(attempt - 1) % len(targets)]
        label = getattr(target, "address", str(target))
        print(f"连接 {label}（第 {attempt}/{attempts} 次，超时 {timeout:.0f}s）...")
        client = BleakClient(target, timeout=timeout)
        try:
            await client.connect()
            print("已连接。")
            return client
        except Exception as exc:  # TimeoutError / BleakError / asyncio.CancelledError
            last_error = exc
            print(f"  失败：{type(exc).__name__}: {exc}")
            try:
                await client.disconnect()
            except Exception:
                pass
            if attempt < attempts:
                await asyncio.sleep(2.5)
    raise RuntimeError(
        f"BLE 连接/服务发现失败：{type(last_error).__name__}: {last_error}\n"
        "按以下顺序排查（**多数情况是 PC 侧，不是设备问题**）：\n"
        "  1) Windows 设置 → 蓝牙和其他设备 → 找到 WANAN-xxxx → **删除设备**（清掉缓存的 GATT 记录），再重试；\n"
        "  2) 关掉手机 nRF Connect / 其它 BLE 工具 / Windows 蓝牙设置页（同一设备只允许一个中心连接）；\n"
        "  3) 关闭再打开蓝牙开关；管理员 PowerShell 可执行：Restart-Service bthserv；\n"
        "  4) 设备管理器 → 蓝牙适配器 → 属性 → 电源管理 → 取消\"允许计算机关闭此设备以节约电源\"；\n"
        "  5) 把开发板靠近电脑（2.4G 拥塞 / BLE 与 WiFi 共存会拖慢服务发现）；\n"
        "  6) 交叉验证：用手机 nRF Connect 连一次——手机能连上就说明是 PC 侧问题；\n"
        "  7) 看设备串口日志：出现 `central connected` = 连到设备了（问题在 PC 的服务发现）；\n"
        "     出现 `central disconnected (reason=0x08 …)` = 链路超时（信号/供电）。"
    )


async def main() -> int:
    parser = argparse.ArgumentParser(description="晚安成电终端 BLE 调试台")
    parser.add_argument("--addr", help="直接指定设备 MAC 地址（跳过扫描）")
    parser.add_argument("--name", help="按广播名选择设备（如 WANAN-1A2B，可只写前缀）")
    parser.add_argument("--index", type=int, help="按扫描结果序号选择（先配合 --list 看列表）")
    parser.add_argument("--list", action="store_true", help="只列出所有可见设备后退出")
    parser.add_argument("--scan-timeout", type=float, default=15.0, help="扫描秒数（默认 15）")
    parser.add_argument("--prefix", default="WANAN", help="广播名前缀（默认 WANAN）")
    parser.add_argument("--timeout", type=float, default=45.0, help="连接超时秒数（默认 45）")
    parser.add_argument("commands", nargs="*", help="要执行的命令（不给则进入交互模式）")
    args = parser.parse_args()

    # 多板场景：不给 --name/--index/--addr 时**绝不自动挑一台**，避免连错设备
    targets = await resolve_target(args)
    if not targets:
        if args.list:
            return 0
        return 1

    try:
        client = await connect_with_retry(targets, args.timeout)
    except RuntimeError as exc:
        print(f"\n{exc}")
        return 1

    async with client:
        console = Console(client)
        try:
            await client.start_notify(NUS_TX, console._on_notify)
        except Exception as exc:
            print(f"订阅通知失败：{type(exc).__name__}: {exc}")
            return 1
        print("已订阅通知。输入 help 查看命令。\n")

        if args.commands:
            for command in args.commands:
                if not await console.run_command(command):
                    break
                await asyncio.sleep(1.0)
            await asyncio.sleep(1.0)
            return 0

        while True:
            try:
                line = await asyncio.to_thread(input, "> ")
            except (EOFError, KeyboardInterrupt):
                print()
                break
            if not await console.run_command(line):
                break
            await asyncio.sleep(0.2)
        await client.stop_notify(NUS_TX)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        print("\n已中断")
