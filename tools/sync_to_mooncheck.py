#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把主工程同步到公开副本 mooncheck/，并**自动避开含脱敏差异的文件**。

为什么需要：mooncheck/ 与主工程只有少数文件"内容故意不同"（把作者真实坐标/指纹/
学号/串口端口换成占位值）。手工整份复制这些文件会把真实信息带进公开副本
（本项目已经踩过两次），所以这里用白名单式复制 + 明确跳过清单来根治。

用法:
    python tools/sync_to_mooncheck.py            # 同步
    python tools/sync_to_mooncheck.py --dry-run  # 只看会做什么
同步完请再跑： python tools/check_repo_hygiene.py --strict
"""
import argparse
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, 'mooncheck')

# Windows 控制台默认 GBK，输出 ⚠ 这类字符会 UnicodeEncodeError 直接崩掉脚本
# （已经踩过：文件复制完成后才崩，看起来像同步失败）。
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except (AttributeError, ValueError):
    pass

# 要同步的目录（相对主工程根）
SYNC_DIRS = ['firmware', 'tools', 'docs']

# 跳过规则：命中即不复制（这些文件公开副本与私有副本**故意不同**，或根本不该入库）
SKIP_SUBSTR = [
    os.path.join('firmware', '.pio'),
    os.path.join('firmware', '.vscode'),
    '__pycache__',
    os.path.join('tools', 'pc_reference', 'config.json'),
    os.path.join('tools', 'pc_reference', 'out'),
    os.path.join('tools', 'check_repo_hygiene.py'),   # 内含作者私有值，绝不外发
    os.path.join('tools', 'extract_burp.ps1'),
    # 公开副本只收录面向用户的说明文档；以下开发者文档只留在主工程 docs/，
    # 不允许再被同步回 mooncheck/（曾出现过回灌）。
    os.path.join('docs', 'api-contract.md'),
    os.path.join('docs', 'ble-protocol.md'),
    os.path.join('docs', 'capture-guide.md'),
    os.path.join('docs', 'firmware-design.md'),
    os.path.join('docs', 'multi-user-design.md'),
    os.path.join('docs', 'network-uestc-wifi.md'),
    os.path.join('docs', 'notify-design.md'),
    os.path.join('docs', 'python-test-design.md'),
    os.path.join('docs', 'python-verification-plan.md'),
    os.path.join('docs', 'release-guide.md'),
    os.path.join('docs', 'test-plan-checkin-path.md'),
    os.path.join('docs', 'assets', 'quickstart', 'README.md'),
]

# 需要"手工合并"的文件（公开副本已脱敏或已按用户视角改写，直接覆盖会泄露/冲掉）
MANUAL_MERGE = [
    'README.md',
    os.path.join('firmware', 'README.md'),
    os.path.join('firmware', 'platformio.ini'),
    os.path.join('firmware', 'src', 'app_config.cpp'),
    os.path.join('docs', 'quickstart.md'),
    os.path.join('docs', 'flashing.md'),
    os.path.join('docs', 'commands.md'),
    os.path.join('docs', 'notify.md'),
    os.path.join('docs', 'multi-account.md'),
    os.path.join('docs', 'faq.md'),
    os.path.join('tools', 'pc_reference', 'README.md'),
    os.path.join('tools', 'pc_reference', 'config.example.json'),
    os.path.join('tools', 'pc_reference', 'portal_sequence.json'),
]


def should_skip(rel):
    norm = rel.replace('/', os.sep)
    for item in SKIP_SUBSTR:
        if item in norm:
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description='同步到公开副本 mooncheck/')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()

    copied, skipped_manual, created_dirs = [], [], set()
    for base in SYNC_DIRS:
        src_dir = os.path.join(ROOT, base)
        if not os.path.isdir(src_dir):
            continue
        for dirpath, dirnames, filenames in os.walk(src_dir):
            dirnames[:] = [d for d in dirnames if d not in ('.pio', '.vscode', '__pycache__')]
            for name in filenames:
                src = os.path.join(dirpath, name)
                rel = os.path.relpath(src, ROOT)
                if should_skip(rel):
                    continue
                # ★ 关键闸门：这些文件的公开副本已脱敏，**绝不允许自动覆盖**
                if rel in MANUAL_MERGE:
                    skipped_manual.append(rel)
                    continue
                dst = os.path.join(DEST, rel)
                parent = os.path.dirname(dst)
                if not os.path.isdir(parent):
                    # 公开副本里还没有这个目录（例如新增的 docs/assets/...）→ 建出来再复制
                    os.makedirs(parent, exist_ok=True)
                    created_dirs.add(os.path.relpath(parent, DEST))
                copied.append(rel)
                if not args.dry_run:
                    shutil.copy2(src, dst)

    print('复制 %d 个文件%s' % (len(copied), '（dry-run，未实际写入）' if args.dry_run else ''))
    if created_dirs:
        print('新建目录: ' + ', '.join(sorted(created_dirs)))
    if skipped_manual:
        print('\n⚠ 以下文件公开副本已脱敏，**不做自动覆盖**，改动后请手工合并：')
        for item in sorted(skipped_manual):
            print('   - ' + item)
    print('\n下一步： python tools/check_repo_hygiene.py --strict')
    return 0


if __name__ == '__main__':
    sys.exit(main())
