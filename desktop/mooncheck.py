#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MoonCheck 桌面版入口（源码运行与 PyInstaller 打包共用）。"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'src'))

from mooncheck_app.__main__ import main  # noqa: E402

if __name__ == '__main__':
    sys.exit(main())
