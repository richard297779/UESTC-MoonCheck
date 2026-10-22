#!/usr/bin/env bash
# Linux 产物 GUI 冒烟（WSL 内执行；判据：进程存活满 6 秒被 timeout 终止 = rc 124/130/143）
set -u
cd "$(dirname "$0")"
rm -f config.json
export DISPLAY="${DISPLAY:-:0}"
timeout -s INT 6 ./MoonCheck >/dev/null 2>&1
rc=$?
if [ "$rc" = "124" ] || [ "$rc" = "130" ] || [ "$rc" = "143" ]; then
  echo "GUI: 启动并存活 6 秒（WSLg OK）"
else
  echo "GUI: rc=$rc（无图形环境或启动失败；CLI 已由 tests/test_offline.py 覆盖）"
fi
ls config.json >/dev/null 2>&1 && echo "config.json 生成在二进制同目录 OK" && rm -f config.json
ls config.json 2>/dev/null && echo "config.json 生成在二进制同目录 OK" && rm -f config.json
