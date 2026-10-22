# -*- coding: utf-8 -*-
"""可执行文件同目录的路径解析。

打包（PyInstaller onefile）后 ``sys.executable`` 就是 MoonCheck.exe 本身，
config.json / 日志都放在它旁边。源码运行时退回到 ``desktop/`` 目录，
方便开发者直接 ``python src/mooncheck_app``。
"""
import os
import sys


def app_dir():
    """可执行文件所在目录（源码运行时为 desktop/ 目录）。"""
    if getattr(sys, 'frozen', False):
        return os.path.dirname(os.path.abspath(sys.executable))
    # .../desktop/src/mooncheck_app/paths.py -> .../desktop
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(os.path.dirname(here))


def config_path():
    """配置文件路径：优先环境变量 MOONCHECK_CONFIG，其次 exe 同目录 config.json。"""
    env = os.environ.get('MOONCHECK_CONFIG')
    if env:
        return os.path.abspath(os.path.expanduser(env))
    return os.path.join(app_dir(), 'config.json')


def logs_dir():
    return os.path.join(app_dir(), 'logs')


def is_frozen():
    return bool(getattr(sys, 'frozen', False))
