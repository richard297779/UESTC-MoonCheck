# -*- coding: utf-8 -*-
"""包入口：python -m mooncheck_app（或 PyInstaller 打包后的 main）。

无参数 = 图形界面（Windows 构建时控制台窗口自动隐藏）；
带参数 = 命令行。
"""
import sys


def _hide_console_on_windows():
    """控制台构建下双击进入 GUI 时，把黑窗口藏掉（--windowed 构建无需此步）。"""
    if sys.platform.startswith('win') and getattr(sys, 'frozen', False):
        try:
            import ctypes
            hwnd = ctypes.windll.kernel32.GetConsoleWindow()
            if hwnd:
                ctypes.windll.user32.ShowWindow(hwnd, 0)  # SW_HIDE
        except Exception:
            pass


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if hasattr(sys.stdout, 'reconfigure'):
        try:
            sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        except Exception:
            pass
    if not argv:
        _hide_console_on_windows()
        from .gui import run_gui
        return run_gui()
    from .cli import main_cli
    return main_cli(argv)


if __name__ == '__main__':
    sys.exit(main())
