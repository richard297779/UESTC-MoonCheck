#!/usr/bin/env bash
# ============================================================
#  MoonCheck Linux 单文件构建（在 Linux/WSL 内运行；产物只能用于 Linux）
#
#  WSL 首次准备（Ubuntu/Debian）：
#    sudo apt update
#    sudo apt install -y python3-venv python3-tk squashfs-tools
#    bash build_linux.sh
#
#  脚本会在 desktop/.venv 里装 pyinstaller（仅构建期依赖，运行时零依赖）。
#  产物：dist/linux/MoonCheck（chmod +x 后可直接拷贝到任意 x86_64 Linux 运行）
# ============================================================
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -d .venv ]; then
  echo "[..] 创建构建虚拟环境 .venv ..."
  python3 -m venv .venv
fi
# shellcheck disable=SC1091
source .venv/bin/activate
python -m pip install -q --upgrade pip
python -m pip install -q -r requirements-build.txt

python -m PyInstaller --clean --noconfirm --distpath dist/linux mooncheck.spec
cp -f config.example.json dist/linux/config.example.json

echo
echo "[OK] 产物：dist/linux/MoonCheck"
echo "     若 WSL 里报缺少共享库，运行: sudo apt install -y libfuse2 squashfs-tools"
