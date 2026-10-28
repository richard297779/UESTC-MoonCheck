# -*- mode: python ; coding: utf-8 -*-
"""MoonCheck 桌面版 PyInstaller 规格（单文件、控制台）。

用法（在 desktop/ 目录，环境内有 pyinstaller）：
    pyinstaller --clean --noconfirm mooncheck.spec
Windows 产物 dist/windows/MoonCheck.exe；Linux 产物 dist/linux/MoonCheck（由构建脚本的 --distpath 决定）。
"""

import os
from PyInstaller.utils.hooks import collect_submodules

_here = os.path.abspath(SPECPATH) if 'SPECPATH' in dir() else os.path.dirname(os.path.abspath(__file__))

APP_NAME = 'MoonCheck'

a = Analysis(
    [os.path.join(_here, 'mooncheck.py')],
    pathex=[os.path.join(_here, 'src')],
    binaries=[],
    datas=[],
    hiddenimports=[
        'mooncheck_app', 'mooncheck_app.aes', 'mooncheck_app.paths',
        'mooncheck_app.config', 'mooncheck_app.http_client', 'mooncheck_app.cas_auth',
        'mooncheck_app.checkin_api', 'mooncheck_app.push', 'mooncheck_app.service',
        'mooncheck_app.cli', 'mooncheck_app.wechat', 'mooncheck_app.wechat_wire', 'mooncheck_app.wechat_transport',
        'mooncheck_app.gui', 'tkinter', 'tkinter.ttk', 'tkinter.filedialog',
        'tkinter.messagebox', 'tkinter.scrolledtext',
    ] + collect_submodules('Crypto', filter=lambda name: not name.startswith('Crypto.SelfTest')),
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name=APP_NAME,
    version=os.path.join(_here, 'version_info.txt') if os.name == 'nt' else None,
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=True,          # 保留控制台：CLI 输出可见；双击时无参数则进入 GUI
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
