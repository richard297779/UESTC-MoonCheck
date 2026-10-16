#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""局域网通知中转（可选，PC 侧运行，标准库实现，无需额外依赖）。

用途：设备不想保存任何公网推送 token 时，把通知 POST 到家里/宿舍的 PC，
由 PC 再决定怎么送到手机（转发给 Bark/ntfy/Server酱，或只做桌面提示）。

设备侧配置：
    notify set url=http://<PC局域网IP>:8770/notify style=2 on=1

PC 侧运行：
    python tools/notify_sink.py                      # 只打印（+ 系统提示音）
    python tools/notify_sink.py --forward https://api.day.app/<你的BarkKEY>/{title}/{body}
    python tools/notify_sink.py --forward https://ntfy.sh/<你的主题>    # 自动按纯文本 POST
    python tools/notify_sink.py --exec "powershell -NoProfile -Command Write-Host {title}"

--forward 的格式与固件 `notifyStyle` 完全一致：URL 里有 {title}/{body} 占位就发 GET 模板，
否则按首行为标题的纯文本 POST（ntfy 约定）。
"""
import argparse
import json
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STATE = {"count": 0, "last": None}


def render_forward(url: str, title: str, body: str, device: str):
    """按固件同一套规则渲染转发请求，返回 (method, url, payload_bytes, content_type)。"""
    if "{" in url:
        head, tmpl = url.split("{", 1)
        tmpl = "{" + tmpl
        filled = tmpl.replace("{title}", urllib.parse.quote(title)).replace(
            "{body}", urllib.parse.quote(body))
        return "GET", head + filled, None, None
    text = f"{title}\n{body}\n".encode("utf-8")
    return "POST", url, text, "text/plain; charset=utf-8"


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):  # noqa: N802
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        raw = self.rfile.read(length) if length > 0 else b""
        title, body, device = "", "", ""
        try:
            data = json.loads(raw.decode("utf-8")) if raw else {}
            title = str(data.get("title", ""))
            body = str(data.get("body", ""))
            device = str(data.get("device", ""))
        except (ValueError, UnicodeDecodeError):
            # 纯文本：首行标题，其余正文
            lines = raw.decode("utf-8", "replace").splitlines() or [""]
            title = lines[0]
            body = "\n".join(lines[1:])
        if not title:
            title = "通知"

        STATE["count"] += 1
        STATE["last"] = title
        stamp = time.strftime("%Y-%m-%d %H:%M:%S")
        print(f"[{stamp}] #{STATE['count']} {title}")
        for line in body.splitlines():
            print(f"        {line}")
        if device:
            print(f"        (device={device})")
        try:
            sys.stdout.flush()
        except Exception:  # noqa: BLE001
            pass

        args = self.server.opts  # type: ignore[attr-defined]
        if args.forward:
            try:
                method, url, payload, ctype = render_forward(args.forward, title, body, device)
                req = urllib.request.Request(url, data=payload, method=method)
                if ctype:
                    req.add_header("Content-Type", ctype)
                with urllib.request.urlopen(req, timeout=8) as resp:
                    print(f"        forwarded -> {resp.status} {resp.read()[:120]!r}")
            except Exception as exc:  # noqa: BLE001
                print(f"        forward FAILED: {exc}")
        if args.exec:
            try:
                cmd = (args.exec.replace("{title}", title)
                       .replace("{body}", body).replace("{device}", device))
                subprocess.run(cmd, shell=True, check=False, timeout=20)
            except Exception as exc:  # noqa: BLE001
                print(f"        exec FAILED: {exc}")
        if args.beep:
            print("\a", end="", flush=True)

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(b'{"code":0,"msg":"ok"}')

    def do_GET(self):  # noqa: N802  —— 浏览器里打开 / 看个状态，方便验收
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.end_headers()
        self.wfile.write(
            f"notify_sink ok\nreceived={STATE['count']}\nlast={STATE['last']}\n".encode("utf-8"))

    def log_message(self, fmt, *a):  # 静音默认访问日志
        pass


def main() -> int:
    p = argparse.ArgumentParser(description="局域网通知中转")
    p.add_argument("--port", type=int, default=8770)
    p.add_argument("--host", default="0.0.0.0")
    p.add_argument("--forward", default=os.environ.get("NOTIFY_FORWARD", ""),
                   help="转发地址（支持 {title}/{body} 占位；留空=只打印）")
    p.add_argument("--exec", dest="exec", default="", help="每条通知执行的命令模板，支持 {title}/{body}/{device}")
    p.add_argument("--beep", action="store_true", help="收到时响一声")
    args = p.parse_args()

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.opts = args  # type: ignore[attr-defined]
    print(f"监听 http://{args.host}:{args.port}/notify  （Ctrl+C 退出）")
    if args.forward:
        print(f"转发到：{args.forward.split('?')[0]}…"[:80])
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
    return 0


if __name__ == "__main__":
    sys.exit(main())
