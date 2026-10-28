"""Windowed double-click entry point (Python + tkinter + bleak required)."""
if __name__ == "__main__":
    try:
        from ble_gui import launch
        launch()
    except Exception:
        import ctypes
        import traceback
        ctypes.windll.user32.MessageBoxW(
            None,
            "启动失败，请在终端运行 python tools/ble_console.py --gui 查看错误。\n\n"
            + traceback.format_exc(),
            "MoonCheck BLE 控制台", 0x10)
