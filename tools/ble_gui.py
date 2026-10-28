"""Tk BLE control panel. All Bluetooth work lives on one asyncio worker thread."""
import asyncio
import concurrent.futures
import json
import queue
import threading
import tkinter as tk
from tkinter import messagebox, ttk
from tkinter.scrolledtext import ScrolledText

import ble_console as protocol


def display_message(message):
    """Only retain masked account identifiers, never credential values."""
    safe = protocol.Console._redact(message)
    data = message.get("data")
    if isinstance(data, list) and message.get("msg") == "accounts":
        for source, target in zip(data, safe["data"]):
            user = str(source.get("user", ""))
            target["user"] = user[:4] + "******" if user else "未配置"
    return safe


def device_defaults(operation, snapshot):
    cfg = snapshot.get("cfg", {})
    location = cfg.get("location", {})
    schedule = cfg.get("schedule", {})
    mapping = {
        "wifi.set": {"ssid": cfg.get("wifiSsid")},
        "loc.set": {"lat": location.get("lat"), "lng": location.get("lng"), "addr": location.get("address")},
        "sched.set": {"at": schedule.get("checkinAt"), "window_end": schedule.get("windowEnd"),
                      "window_start": schedule.get("windowStart"), "jitter_s": schedule.get("jitterSeconds")},
        "auto.set": {"enabled": schedule.get("autoDaily")},
        "multi.set": {"gap_s": cfg.get("accountGap")},
        "name.set": {"name": cfg.get("label")},
        "notify.set": {k: snapshot.get("notification", {}).get(k) for k in ("on", "style", "gap", "quota")},
    }
    return {k: v for k, v in mapping.get(operation, {}).items() if v is not None}


# title, firmware operation, fields (key, label, type, default), confirmation
OPERATIONS = {
    "状态": [
        ("设备信息", "hello", [], ""),
        ("配置与运行状态", "status", [], ""),
        ("设备时间", "test.time", [], ""),
        ("前置体检", "test.probe", [], ""),
        ("近期日志", "logs", [("n", "行数（1–100）", "int", "30")], ""),
    ],
    "网络": [
        ("扫描 Wi-Fi", "wifi.scan", [], ""),
        ("保存并连接 Wi-Fi", "wifi.set", [("ssid", "网络名", "text", ""), ("pass", "密码（开放网络留空）", "secret", "")], ""),
        ("校园网连通性", "test.http", [("url", "检测地址", "text", "http://connect.rom.miui.com/generate_204")], ""),
        ("自定义 HTTP 测试", "test.http", [("url", "URL", "secret", "")], "将由设备访问此地址。"),
        ("修改广播名", "name.set", [("name", "标签（A-Za-z0-9-_，最多12字）", "text", "")], ""),
    ],
    "账号与位置": [
        ("单人账号", "auth.set", [("user", "学号", "secret", ""), ("pass", "密码", "secret", ""), ("bfp", "浏览器指纹（可空）", "secret", "")], ""),
        ("打卡位置", "loc.set", [("lat", "纬度", "float", ""), ("lng", "经度", "float", ""), ("addr", "地址（可空）", "text", "")], ""),
        ("账号列表", "account.list", [], ""),
        ("添加或覆盖账号", "account.add", [("label", "标签", "text", ""), ("user", "学号", "secret", ""), ("pass", "密码", "secret", ""), ("bfp", "指纹（可空）", "secret", "")], "同标签账号将被覆盖；位置沿用设备配置。"),
        ("启用或禁用账号", "account.enable", [("label", "标签", "text", ""), ("on", "启用", "bool", True)], ""),
        ("删除账号", "account.remove", [("label", "标签", "text", "")], "将删除该账号。"),
        ("清空账号表", "account.clear", [], "将清空账号表；设备将回退到单人账号。"),
    ],
    "微信授权": [
        ("授权状态", "wx.status", [], ""),
        ("导入本人授权", "wx.import", [("slot", "槽位（0–5，按账号顺序）", "int", "0"), ("openid", "OpenID", "secret", ""), ("access", "Access Token", "secret", ""), ("refresh", "Refresh Token", "secret", ""), ("expiresAt", "Unix 到期秒（0=先刷新）", "int", "0")], ""),
        ("测试取票", "wx.test", [("slot", "槽位（0–5）", "int", "0")], "将访问真实微信授权接口，只取票，不提交打卡。"),
    ],
    "调度与执行": [
        ("保存时间窗口", "sched.set", [("at", "计划执行 HH:MM", "text", "22:00"), ("window_start", "窗口开始（设备未提供时可空）", "text", ""), ("window_end", "窗口结束 HH:MM", "text", "23:30"), ("jitter_s", "随机延迟秒", "int", "30")], ""),
        ("自动打卡开关", "auto.set", [("enabled", "启用每日自动打卡", "bool", False)], "将修改设备的每日调度开关。"),
        ("多人间隔", "multi.set", [("gap_s", "间隔秒（0–600）", "int", "20")], ""),
        ("登录测试", "test.login", [("force", "窗口外也登录", "bool", False)], "将使用真实账号登录。"),
        ("全链路测试", "test.chain", [("wait_s", "等待放行秒（0–900）", "int", "0"), ("force", "强制登录", "bool", False)], "将使用真实账号访问平台。"),
        ("立即打卡", "test.checkin", [("force", "跳过今日已打卡检查", "bool", False)], "将提交真实打卡请求。"),
        ("多人执行", "test.multi", [("probe", "只体检不提交（仍会登录）", "bool", True), ("force", "强制执行", "bool", False)], "将访问真实账号；取消只体检后会提交打卡。"),
    ],
    "通知": [
        ("通知状态", "notify.status", [], ""),
        ("保存默认通道", "notify.set", [("url", "推送 URL（可空，保留设备原值）", "secret", ""), ("style", "方式 0=GET / 1=文本 / 2=JSON", "int", "2"), ("on", "启用", "bool", True), ("gap", "去重秒", "int", "60"), ("quota", "每日上限（0=不限）", "int", "40")], ""),
        ("添加或覆盖额外通道", "notify.add", [("label", "标签", "text", ""), ("url", "推送 URL", "secret", ""), ("style", "方式 0/1/2", "int", "2"), ("accounts", "关联账号（逗号分隔或 all）", "text", "all"), ("on", "启用", "bool", True)], ""),
        ("通道开关", "notify.enable", [("label", "标签", "text", ""), ("on", "启用", "bool", True)], ""),
        ("关联账号", "notify.link", [("label", "标签", "text", ""), ("accounts", "账号（逗号分隔或 all）", "text", "all")], ""),
        ("删除通道", "notify.remove", [("label", "标签", "text", "")], "将删除该通知通道。"),
        ("测试通知", "test.notify", [("chan", "额外通道标签（可空）", "text", ""), ("body", "测试文本", "text", "MoonCheck 测试通知")], "将立即发送真实通知。"),
    ],
    "开发者": [
        ("清空设备 Cookie", "session.clear", [], "将清空设备登录会话。"),
        ("注入 SESSION", "session.set", [("value", "SESSION", "secret", "")], ""),
        ("恢复出厂", "factory.reset", [], "将擦除设备所有配置和凭据，无法撤销。"),
    ],
}


def build_payload(spec, values):
    """Typed form conversion; preserve spaces in SSIDs/passwords."""
    payload = {"op": spec[1]}
    for key, label, kind, _default in spec[2]:
        value = values[key]
        if kind == "int":
            value = int(value)
        elif kind == "float":
            import math
            value = float(value)
            if not math.isfinite(value):
                raise ValueError(f"{label}必须是有限数字")
        elif kind == "bool":
            value = bool(value)
        elif not value and "可空" not in label and key != "pass":
            raise ValueError(f"请填写{label}")
        if value == "" and key in ("bfp", "chan"):
            continue
        if value == "" and key == "url" and payload["op"] == "notify.set":
            continue
        if value == "" and key == "window_start":
            continue
        payload[key] = value
    for key, low, high in (("slot", 0, 5), ("expiresAt", 0, 2**63-1),
                           ("n", 1, 100), ("gap_s", 0, 600), ("wait_s", 0, 900),
                           ("style", 0, 2), ("lat", -90, 90), ("lng", -180, 180)):
        if key in payload and not low <= payload[key] <= high:
            raise ValueError(f"{key} 应在 {low}–{high} 之间")
    if payload["op"] == "factory.reset":
        payload["confirm"] = "YES"
    return payload


class Session(protocol.Console):
    def __init__(self, client, output, on_message):
        super().__init__(client, output, on_message)
        self.waiters = {}
        self.max_bytes = 511

    def abort(self):
        for waiter in self.waiters.values():
            if not waiter.done():
                waiter.set_exception(ConnectionError("设备断开"))

    def _print_json(self, text):
        super()._print_json(text)
        try:
            message = json.loads(text)
        except ValueError:
            return
        if isinstance(message, dict):
            waiter = self.waiters.get(message.get("id"))
            if waiter and not waiter.done() and message.get("evt") in ("ok", "err", "error"):
                waiter.set_result(message)

    async def request(self, payload, timeout=60):
        payload = dict(payload)
        payload["id"] = self.next_id
        request_id = self.next_id
        if len(json.dumps(payload, ensure_ascii=False).encode("utf-8")) > self.max_bytes:
            raise ValueError(f"超过设备声明的 {self.max_bytes} 字节指令上限")
        waiter = asyncio.get_running_loop().create_future()
        self.waiters[request_id] = waiter
        try:
            await self.send(payload)
            message = await asyncio.wait_for(waiter, timeout)
            if message.get("evt") != "ok":
                raise ValueError("设备拒绝此操作，详见脱敏回执")
            if payload["op"] == "hello":
                limit = message.get("data", {}).get("maxCommandBytes", 511)
                self.max_bytes = min(4095, max(1, int(limit)))
            return message
        finally:
            self.waiters.pop(request_id, None)


class Backend:
    def __init__(self, events):
        self.events = events
        self.client = self.session = None
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()
        pending = asyncio.all_tasks(self.loop)
        for task in pending:
            task.cancel()
        self.loop.run_until_complete(asyncio.gather(*pending, return_exceptions=True))
        self.loop.close()

    async def scan(self):
        if protocol.BleakScanner is None:
            raise RuntimeError("缺少 bleak：请运行 python -m pip install -r tools/requirements-ble.txt")
        found = await protocol.BleakScanner.discover(timeout=8, return_adv=True)
        return [(adv.local_name or dev.name or "未命名", dev.address, adv.rssi, dev)
                for dev, adv in found.values()
                if (adv.local_name or dev.name or "").upper().startswith("WANAN")
                or "6e400001-b5a3-f393-e0a9-e50e24dcca9e" in adv.service_uuids]

    async def connect(self, target):
        await self.disconnect()
        if protocol.BleakClient is None:
            raise RuntimeError("请先安装 tools/requirements-ble.txt 中的 bleak")
        client = protocol.BleakClient(target, timeout=45,
            disconnected_callback=lambda c: self.loop.call_soon_threadsafe(self._disconnected, c))
        self.client = client
        try:
            await client.connect()
            self.session = Session(client, lambda text: self.events.put(("log", text)),
                                   lambda obj: self.events.put(("message", display_message(obj))))
            await client.start_notify(protocol.NUS_TX, self.session._on_notify)
            await self.session.request({"op": "hello"})
            await self.refresh()
        except BaseException:
            await self.disconnect()
            raise

    def _disconnected(self, client):
        if self.client is client:
            if self.session:
                self.session.abort()
            self.session = None
            self.events.put(("disconnected", None))

    async def disconnect(self):
        client, self.client = self.client, None
        if self.session:
            self.session.abort()
        self.session = None
        if client:
            await asyncio.wait_for(client.disconnect(), 8)

    async def request(self, payload):
        if not self.session or not self.client.is_connected:
            raise RuntimeError("请先连接设备")
        timeout = max(60, int(payload.get("wait_s", 0)) + 90)
        result = await self.session.request(payload, timeout)
        if payload["op"].endswith((".set", ".add", ".remove", ".enable", ".clear", ".import", ".link")):
            try:
                await self.refresh()
            except Exception:
                self.events.put(("refresh_failed", None))
        return result

    async def refresh(self):
        if not self.session:
            raise RuntimeError("尚未连接")
        for operation in ("status", "account.list", "wx.status", "notify.status"):
            await self.session.request({"op": operation})


class Application:
    def __init__(self, root):
        self.root = root
        self.events = queue.Queue()
        self.backend = Backend(self.events)
        self.busy = self.connected = self.closing = False
        self.devices = []
        self.snapshot = {}
        self.accounts = []
        self.developer = tk.BooleanVar(value=False)
        root.title("MoonCheck · 蓝牙控制台")
        root.geometry("1120x820")
        root.minsize(900, 680)
        style = ttk.Style(root)
        if "clam" in style.theme_names():
            style.theme_use("clam")
        style.configure("TFrame", background="#f3f6fb")
        style.configure("TLabel", background="#f3f6fb", font=("Microsoft YaHei UI", 10))
        style.configure("TButton", padding=(12, 7))
        style.configure("Treeview", rowheight=32, font=("Microsoft YaHei UI", 10))
        style.configure("Treeview.Heading", font=("Microsoft YaHei UI", 10, "bold"))
        style.configure("TNotebook.Tab", padding=(12, 8))
        outer = ttk.Frame(root, padding=18)
        outer.pack(fill="both", expand=True)
        ttk.Label(outer, text="MoonCheck 蓝牙控制台", font=("Microsoft YaHei UI", 18, "bold")).pack(anchor="w")
        ttk.Label(outer, text="连接开发板 · 配置网络与账号 · 查看运行状态", foreground="#526275").pack(anchor="w", pady=(4, 12))
        bar = ttk.Frame(outer)
        bar.pack(fill="x")
        self.device = ttk.Combobox(bar, state="readonly", width=45)
        self.device.pack(side="left", padx=(0, 8))
        self.scan_button = ttk.Button(bar, text="扫描设备", command=lambda: self.submit(self.backend.scan(), "scan"))
        self.scan_button.pack(side="left")
        self.connect_button = ttk.Button(bar, text="连接", command=self.connect)
        self.connect_button.pack(side="left", padx=6)
        ttk.Button(bar, text="断开", command=lambda: self.submit(self.backend.disconnect(), "disconnect")).pack(side="left")
        ttk.Button(bar, text="帮助", command=self.help).pack(side="right")
        ttk.Checkbutton(bar, text="开发者选项", variable=self.developer, command=self.toggle_developer).pack(side="right", padx=10)
        self.status = tk.StringVar(value="未连接 · 点击扫描设备")
        ttk.Label(outer, textvariable=self.status, foreground="#245a91").pack(anchor="w", pady=10)
        self.runtime = tk.StringVar(value="运行状态：连接后选择“配置与运行状态”查询")
        ttk.Label(outer, textvariable=self.runtime, foreground="#526275").pack(anchor="w", pady=(0, 8))
        book = self.book = ttk.Notebook(outer)
        book.pack(fill="both", expand=True)
        overview = ttk.Frame(book, padding=18)
        book.add(overview, text="设备概览")
        self.overview = tk.StringVar(value="欢迎使用 MoonCheck\n\n请先扫描并连接设备，配置和账号将自动读取。")
        ttk.Label(overview, textvariable=self.overview, font=("Microsoft YaHei UI", 12), justify="left").pack(anchor="w", pady=10)
        ttk.Button(overview, text="刷新设备数据", command=self.refresh).pack(anchor="w", pady=10)
        accounts_tab = ttk.Frame(book, padding=14)
        book.add(accounts_tab, text="我的账号")
        ttk.Label(accounts_tab, text="选择账号后，可到“账号与位置”启用、停用或删除。密码和完整学号不回读。").pack(anchor="w", pady=(0, 10))
        self.account_table = ttk.Treeview(accounts_tab, columns=("label", "user", "enabled", "auth", "last"), show="headings", height=9)
        for key, label in zip(self.account_table["columns"], ("账号标签", "学号", "参与打卡", "密码已配置", "最近结果")):
            self.account_table.heading(key, text=label)
            self.account_table.column(key, width=150, stretch=True)
        self.account_table.pack(fill="both", expand=True)
        self.account_table.bind("<<TreeviewSelect>>", self.select_account)
        self.account_hint = tk.StringVar(value="连接后自动获取账号列表")
        ttk.Label(accounts_tab, textvariable=self.account_hint).pack(anchor="w", pady=8)
        ttk.Button(accounts_tab, text="刷新账号与状态", command=self.refresh).pack(anchor="e")
        self.forms = []
        self.developer_tabs = []
        results_tab = ttk.Frame(book, padding=16)
        book.add(results_tab, text="查询结果")
        ttk.Label(results_tab, text="这里展示设备返回的查询结果；完整协议回执可在开发者选项中查看。").pack(anchor="w", pady=8)
        self.results = ScrolledText(results_tab, height=14, wrap="word", state="disabled", font=("Microsoft YaHei UI", 11))
        self.results.pack(fill="both", expand=True)
        for category, specs in OPERATIONS.items():
            specs = list(specs)
            if category == "状态":
                specs = [s for s in specs if s[1] != "logs"]
            if category == "开发者":
                specs = OPERATIONS["状态"][-1:] + specs
            tab = ttk.Frame(book, padding=12)
            book.add(tab, text=category)
            if category == "开发者":
                self.developer_tabs.append(tab)
            choice = ttk.Combobox(tab, values=[s[0] for s in specs], state="readonly", width=34)
            choice.pack(anchor="w")
            form = ttk.Frame(tab)
            form.pack(fill="x", pady=8)
            choice.current(0)
            state = {"choice": choice, "form": form, "specs": specs}
            self.forms.append(state)
            choice.bind("<<ComboboxSelected>>", lambda _e, s=state: self.render(s))
            self.render(state)
        tab = ttk.Frame(book, padding=12)
        book.add(tab, text="高级 JSON")
        self.developer_tabs.append(tab)
        ttk.Label(tab, text="完整协议入口；仅发送已理解的 JSON。敏感输入不会保存到磁盘。").pack(anchor="w")
        self.raw = ScrolledText(tab, height=5, font=("Consolas", 11))
        self.raw.pack(fill="x", pady=8)
        self.raw.insert("1.0", '{"op":"status"}')
        ttk.Button(tab, text="发送 JSON…", command=self.send_raw).pack(anchor="w")
        receipts = ttk.Frame(book, padding=12)
        book.add(receipts, text="完整回执")
        self.developer_tabs.append(receipts)
        ttk.Label(receipts, text="脱敏回执 · 仅开发者模式收集 · 最多约 500 行 · 关闭或断开清空").pack(anchor="w", pady=6)
        self.log = ScrolledText(receipts, height=10, state="disabled", font=("Consolas", 10))
        self.log.pack(fill="both", expand=True)
        ttk.Button(receipts, text="清空回执", command=self.clear_log).pack(anchor="e", pady=(4, 0))
        self.toggle_developer()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(80, self.poll)

    def toggle_developer(self):
        for tab in self.developer_tabs:
            if self.developer.get():
                self.book.add(tab)
            else:
                self.book.hide(tab)
        if not self.developer.get():
            self.clear_log()
            self.raw.delete("1.0", "end")

    def refresh(self):
        if self.connected and not self.busy:
            self.submit(self.backend.refresh(), "refresh")
        else:
            self.status.set("请先连接设备，并等待当前操作完成")

    def reset_device(self):
        self.snapshot.clear()
        self.accounts = []
        self.account_table.delete(*self.account_table.get_children())
        self.account_hint.set("未连接 · 账号数据已清空")
        self.overview.set("设备已断开，请重新连接。")
        self.show_result("设备已断开，查询结果已清空。")
        self.raw.delete("1.0", "end")
        for state in self.forms:
            self.render(state)

    def select_account(self, _event=None):
        selected = self.account_table.selection()
        if not selected:
            return
        account = self.accounts[int(selected[0])]
        for state in self.forms:
            operation = state["specs"][state["choice"].current()][1]
            if operation.startswith("account.") and "label" in state["vars"]:
                state["vars"]["label"].set(account.get("label", ""))
                if "on" in state["vars"]:
                    state["vars"]["on"].set(bool(account.get("enabled")))

    def consume_message(self, message):
        data = message.get("data")
        title = message.get("msg", "")
        if data is not None:
            self.show_result(self.readable_result(data))
        if title == "status" and isinstance(data, dict):
            self.snapshot.update(data)
            cfg, net = data.get("cfg", {}), data.get("net", {})
            schedule, last = cfg.get("schedule", {}), data.get("last", {})
            runtime = data.get("runtime", {})
            self.overview.set(
                f"设备  {cfg.get('device', '—')}     固件  {data.get('fw', '—')}\n\n"
                f"网络  {net.get('ssid') or '未连接'}     IP  {net.get('ip', '—')}\n\n"
                f"设备时间  {net.get('deviceTime', '—')}\n\n"
                f"自动打卡  {'已开启' if schedule.get('autoDaily') else '已关闭'}     计划时间  {schedule.get('checkinAt', '—')}\n\n"
                f"下次执行  {data.get('schedule', {}).get('nextRun', '—')}\n\n"
                f"最近结果  {last.get('date', '')}  {last.get('message') or '暂无记录'}\n\n"
                f"内存峰值  {runtime.get('heapPeakUsedBytes', '—')} B     可用内存  {runtime.get('heapFreeBytes', '—')} B")
        elif title == "accounts" and isinstance(data, list):
            self.accounts = data
            self.account_table.delete(*self.account_table.get_children())
            for index, account in enumerate(data):
                self.account_table.insert("", "end", iid=str(index), values=(
                    account.get("label", ""), account.get("user", ""),
                    "启用" if account.get("enabled") else "停用",
                    "已配置" if account.get("passwordSet") else "未配置",
                    f"{account.get('lastDate', '')} {account.get('lastMsg', '')}"))
            self.account_hint.set(f"共 {len(data)} 个账号，{sum(bool(a.get('enabled')) for a in data)} 个启用"
                                  if data else "账号表为空，设备使用单人账号；可在“账号与位置”设置。")
        elif title == "notify" and isinstance(data, dict):
            notification = dict(data)
            notification["style"] = {"get": 0, "post_text": 1, "post_json": 2}.get(data.get("style"), 2)
            self.snapshot["notification"] = notification
        for state in self.forms:
            if not state.get("dirty"):
                operation = state["specs"][state["choice"].current()][1]
                defaults = device_defaults(operation, self.snapshot)
                for key, value in defaults.items():
                    if key in state["vars"]:
                        state["vars"][key].set(value)
                state["dirty"] = False

    def show_result(self, text):
        self.results.configure(state="normal")
        self.results.delete("1.0", "end")
        self.results.insert("1.0", text)
        self.results.configure(state="disabled")

    @staticmethod
    def readable_result(value, depth=0):
        labels = {"slots": "微信授权槽位", "slot": "槽位", "configured": "已配置", "present": "已保存",
                  "expiresAt": "到期时间（Unix 秒）", "expired": "已过期", "enabled": "启用",
                  "label": "账号 / 通道标签", "user": "学号", "passwordSet": "密码已配置",
                  "fingerprintSet": "指纹已配置", "lastDate": "最近执行日期", "lastMsg": "最近结果",
                  "ssid": "网络名", "rssi": "信号强度", "channel": "频道", "aps": "可用网络",
                  "on": "启用通知", "style": "通知方式", "gap": "去重间隔（秒）", "quota": "每日上限",
                  "sentToday": "今日已发送", "queued": "待发送", "channels": "额外通道",
                  "url": "推送地址（隐藏）", "timeSynced": "时间已同步", "deviceTime": "设备时间",
                  "inWindow": "在打卡窗口内", "accounts": "关联账号", "location": "位置已配置"}
        indent = "  " * depth
        if isinstance(value, dict):
            return "\n".join(indent + labels.get(k, k) + ("：\n" + Application.readable_result(v, depth + 1)
                            if isinstance(v, (dict, list)) else "：" + Application.readable_result(v))
                            for k, v in value.items())
        if isinstance(value, list):
            return "\n\n".join(indent + f"第 {i+1} 项\n" + Application.readable_result(v, depth + 1)
                                for i, v in enumerate(value)) or "暂无记录"
        if isinstance(value, bool):
            return "是" if value else "否"
        return str(value) if value is not None else "未提供"

    def render(self, state):
        for widget in state["form"].winfo_children():
            widget.destroy()
        spec = state["specs"][state["choice"].current()]
        state["vars"] = {}
        state["dirty"] = False
        defaults = device_defaults(spec[1], self.snapshot)
        ttk.Label(state["form"], text="可读取字段以设备回执为准；密码、令牌需重新输入。未提供的配置不会伪装为设备当前值。", foreground="#526275").grid(row=0, column=0, columnspan=2, sticky="w", pady=(6, 12))
        for row, (key, label, kind, default) in enumerate(spec[2], start=1):
            default = defaults.get(key, default)
            if spec[1] in ("sched.set", "loc.set", "wifi.set", "multi.set", "notify.set", "name.set") and key not in defaults and kind not in ("secret", "bool"):
                default = ""
            var = tk.BooleanVar(value=default) if kind == "bool" else tk.StringVar(value=default)
            state["vars"][key] = var
            var.trace_add("write", lambda *_args, s=state: s.update(dirty=True))
            ttk.Label(state["form"], text=label).grid(row=row, column=0, sticky="w", padx=(0, 15), pady=3)
            if kind == "bool":
                widget = ttk.Checkbutton(state["form"], variable=var)
            else:
                widget = ttk.Entry(state["form"], textvariable=var, width=55, show="•" if kind == "secret" else "")
            widget.grid(row=row, column=1, sticky="ew", pady=3)
        state["form"].columnconfigure(1, weight=1)
        ttk.Button(state["form"], text="执行 / 保存", command=lambda: self.send_form(state)).grid(row=len(spec[2])+1, column=1, sticky="e", pady=8)
        self.select_account()

    def send_form(self, state):
        if self.busy or not self.connected:
            messagebox.showinfo("尚不可操作", "请先连接设备，并等待上一操作完成。")
            return
        spec = state["specs"][state["choice"].current()]
        try:
            payload = build_payload(spec, {k: v.get() for k, v in state["vars"].items()})
        except (ValueError, TypeError):
            messagebox.showerror("输入有误", "请检查必填字段、数字格式与允许范围。")
            return
        if spec[3] and not messagebox.askyesno(spec[0], spec[3] + "\n确认继续？"):
            return
        self.submit(self.backend.request(payload), "request")
        for key, _label, kind, _default in spec[2]:
            if kind == "secret":
                state["vars"][key].set("")
        state["dirty"] = False

    def send_raw(self):
        try:
            payload = json.loads(self.raw.get("1.0", "end"))
            if not isinstance(payload, dict) or not isinstance(payload.get("op"), str):
                raise ValueError()
        except ValueError:
            messagebox.showerror("JSON 无效", "请输入含字符串 op 字段的 JSON 对象。")
            return
        if self.busy or not self.connected:
            messagebox.showinfo("尚不可操作", "请先连接并等待上一操作完成。")
            return
        if messagebox.askyesno("发送高级指令", "高级指令可能更改配置、提交打卡或擦除数据。确认执行？"):
            self.submit(self.backend.request(payload), "request")
            self.raw.delete("1.0", "end")

    def connect(self):
        if self.busy:
            return
        index = self.device.current()
        if index < 0:
            messagebox.showinfo("选择设备", "请扫描后选择一台设备。")
            return
        self.reset_device()
        self.submit(self.backend.connect(self.devices[index][3]), "connect")

    def submit(self, coroutine, kind):
        if self.busy or self.closing:
            coroutine.close()
            return
        self.busy = True
        self.status.set("处理中… 等待设备响应；请勿重复操作")
        future = asyncio.run_coroutine_threadsafe(coroutine, self.backend.loop)
        self.future = future
        def done(result):
            try:
                value = result.result()
                self.events.put(("done", (kind, value, None)))
            except (Exception, concurrent.futures.CancelledError) as exc:
                # No backend exception text: it can contain credentials/HTTP content.
                self.events.put(("done", (kind, None, type(exc).__name__)))
        future.add_done_callback(done)

    def clear_log(self):
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    def poll(self):
        for _ in range(100):
            try:
                kind, value = self.events.get_nowait()
            except queue.Empty:
                break
            if kind == "log":
                if not self.developer.get():
                    continue
                self.log.configure(state="normal")
                self.log.insert("end", value + "\n")
                lines = int(self.log.index("end-1c").split(".")[0])
                if lines > 500:
                    self.log.delete("1.0", f"{lines-500}.0")
                self.log.see("end")
                self.log.configure(state="disabled")
            elif kind == "disconnected":
                self.connected = False
                self.reset_device()
                self.clear_log()
                self.runtime.set("运行状态：设备已断开")
                self.status.set("设备已断开；未完成操作不会自动重放")
            elif kind == "refresh_failed":
                self.status.set("操作已确认，但刷新失败；请手动刷新设备数据")
            elif kind == "message":
                self.consume_message(value)
                data = value.get("data", {})
                if isinstance(data, dict) and isinstance(data.get("runtime"), dict):
                    runtime = data["runtime"]
                    wifi = data.get("net", {})
                    self.runtime.set(f"网络：{wifi.get('ssid', '未知')}  |  "
                                     f"堆峰值：{runtime.get('heapPeakUsedBytes', '?')} B  |  "
                                     f"可用堆：{runtime.get('heapFreeBytes', '?')} B  |  "
                                     f"PSRAM 可用：{runtime.get('psramFreeBytes', '?')} B")
            elif kind == "done":
                operation, result, error = value
                self.busy = False
                if error:
                    self.status.set(f"操作失败（{error}）；检查蓝牙、设备回执与帮助后重试")
                    if operation == "connect":
                        self.connected = False
                        self.reset_device()
                else:
                    if operation == "scan":
                        self.devices = sorted(result, key=lambda d: -(d[2] or -999))
                        self.device["values"] = [f"{d[0]}  |  {d[1]}  |  {d[2]} dBm" for d in self.devices]
                        if self.devices:
                            self.device.current(0)
                        else:
                            self.device.set("")
                    elif operation == "connect":
                        self.connected = True
                    elif operation == "disconnect":
                        self.connected = False
                        self.reset_device()
                        self.clear_log()
                        self.runtime.set("运行状态：设备已断开")
                    self.status.set((f"发现 {len(self.devices)} 台设备 · 请选择后连接" if operation == "scan" else
                                     "设备已确认操作完成" if self.connected else "未连接 · 扫描并选择设备"))
        if not self.closing:
            self.root.after(80, self.poll)

    def help(self):
        window = tk.Toplevel(self.root)
        window.title("使用帮助")
        text = ScrolledText(window, width=90, height=30, wrap="word")
        text.pack(fill="both", expand=True)
        text.insert("1.0", "启动：安装 Python 3.10+（含 tkinter）与 bleak，然后双击 ble_console.pyw。\n"
                    "1. 关闭手机 APK 等 BLE 连接，打开电脑蓝牙；扫描并核对设备地址后连接。\n"
                    "2. 先配置 Wi-Fi、账号与坐标，再导入账号本人微信授权；按账号顺序选择槽位。\n"
                    "3. 查看状态/前置体检/授权状态，按需测试取票。真实打卡和通知会先确认。\n"
                    "4. 等待设备回执；超时结果不确定，请查询状态，切勿盲目重复提交。\n"
                    "5. 页面字段不保存；密码及令牌发送后清空。回执脱敏，但设备自由文本日志仍可能含个人信息，请勿直接分享。\n"
                    "6. 连接失败：关闭其他 BLE 客户端，在系统蓝牙设置删除旧配对，再开关蓝牙重试。\n"
                    "7. 没有扫描结果：确认供电、距离与蓝牙适配器；Linux 还需系统 BlueZ 权限。\n"
                    "8. GUI 只控制 ESP32，不等同于独立运行打卡的 desktop 程序。\n\n"
                    + protocol.HELP_TEXT)
        text.configure(state="disabled")

    def close(self):
        self.closing = True
        if getattr(self, "future", None):
            self.future.cancel()
        future = asyncio.run_coroutine_threadsafe(self.backend.disconnect(), self.backend.loop)
        def finish():
            if future.done():
                self.backend.loop.call_soon_threadsafe(self.backend.loop.stop)
                self.root.destroy()
            else:
                self.root.after(80, finish)
        finish()


def launch():
    root = tk.Tk()
    Application(root)
    if protocol.BleakClient is None:
        messagebox.showwarning("需要安装蓝牙依赖", "请打开终端执行：\npython -m pip install -r tools/requirements-ble.txt\n\n安装后重新启动。")
    root.mainloop()
