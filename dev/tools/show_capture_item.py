#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""脱敏查看抓包条目：只显示结构与请求头，不显示 Cookie / 令牌的**值**。

用途：分析"浏览器如何进入微应用"时，需要看请求头、状态码与 Location，
      但抓包文件里含有等价于账号通行证的 Cookie，不能整篇打印。

用法:
    python tools/show_capture_item.py capture/analysis/items/0099_....txt
    python tools/show_capture_item.py capture/analysis/items/0099_*.txt --body-headers
"""
import argparse
import glob
import re
import sys

SECRET_HEADERS = {"cookie", "set-cookie", "authorization", "proxy-authorization"}
HEX_TOKEN = re.compile(r"[0-9a-fA-F]{16,}")
COOKIE_NAME = re.compile(r"(?:^|;\s*)([A-Za-z0-9_\-\.]+)=")


def redact(name: str, value: str) -> str:
    """隐藏敏感值，只留结构。"""
    if name.lower() in SECRET_HEADERS:
        names = []
        for item in COOKIE_NAME.findall(value):
            if item not in names:
                names.append(item)
        return "<" + ",".join(names) + ">"
    value = HEX_TOKEN.sub("<hex>", value)
    if len(value) > 160:
        value = value[:160] + "…"
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description="脱敏查看抓包条目")
    parser.add_argument("paths", nargs="+", help="条目文件（支持通配符）")
    parser.add_argument("--max-lines", type=int, default=60, help="每个文件最多显示多少行")
    args = parser.parse_args()

    files = []
    for pattern in args.paths:
        files.extend(sorted(glob.glob(pattern)) or [pattern])

    for path in files:
        print("=" * 78)
        print(f"# {path}")
        try:
            text = open(path, "r", encoding="utf-8", errors="replace").read()
        except OSError as exc:
            print(f"  读取失败: {exc}")
            continue

        shown = 0
        for raw in text.splitlines():
            if shown >= args.max_lines:
                print("  …（已截断）")
                break
            line = raw.rstrip()
            if not line:
                continue
            match = re.match(r"^([A-Za-z0-9\-]{2,32}):\s*(.*)$", line)
            if match:
                name, value = match.group(1), match.group(2)
                print(f"  {name}: {redact(name, value)}")
            else:
                # 非头部行：只做十六进制令牌脱敏，避免漏出会话
                print(f"  {HEX_TOKEN.sub('<hex>', line)[:200]}")
            shown += 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
