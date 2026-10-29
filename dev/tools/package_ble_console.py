"""Build and verify a self-contained BLE console source ZIP, without uploading."""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SOURCES = (
    'tools/ble_console.py', 'tools/ble_console.pyw', 'tools/ble_gui.py',
    'tools/ble_dashboard.py', 'tools/console_model.py', 'tools/qr_authorization.py',
    'tools/qr_dialog.py', 'tools/requirements-ble.txt', 'LICENSE', 'VERSION',
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def build_archive(output, version, source_commit, source_dirty, repository='richard297779/UESTC-MoonCheck'):
    assert re.fullmatch(r'\d+\.\d+\.\d+', version), 'Invalid version'
    assert re.fullmatch(r'[0-9a-f]{40}', source_commit), 'Invalid source commit'
    assert (ROOT / 'VERSION').read_text().strip() == version, 'BLE version must match project VERSION'
    files = {name: (ROOT / name).read_bytes() for name in SOURCES}
    files['PACKAGE.md'] = f'''# MoonCheck {version} 电脑蓝牙控制台

这是通过 BLE 控制 ESP32 的 Python 图形界面；不包含独立打卡引擎。
需要 Python 3.10+（含 tkinter）及电脑蓝牙，在解压根目录执行：

    python -m pip install -r tools/requirements-ble.txt

将 .pyw 关联到同一 Python 环境的 pythonw.exe，再双击 tools/ble_console.pyw。
也可执行 python tools/ble_console.py --gui；原命令行使用 --cli。

扫描并核对本人设备后连接。四栏目为设备概览、账号列表、查询与配置、开发者完整回执。
账号修改留空保留；新建须填标签、学号、密码与浏览器指纹。微信扫码先匹配目标，确认后只填表。
登录、链路检查、打卡和演练需勾选目标；已禁用账号须先启用。结果以设备回执为准，超时不自动重放。

请配合 {version} 固件（协议 1，account-v2 / selected-execution）。旧固件可查看原状态，新功能提示升级。
学号在账号表完整显示，密码、指纹与令牌不回读；敏感输入和日志不写入磁盘。
不读取系统浏览器 Cookie。扫码需电脑联网，实际授权必须由账号本人完成。
依赖通过 pip 安装，ZIP 不含 Python、第三方库、密钥或本机配置。

使用指南：https://github.com/{repository}/blob/v{version}/docs/ble-gui.md
源码及逐文件摘要见 manifest.json。用户已确认基本功能；未提供逐设备兼容矩阵。
'''.encode('utf-8')
    manifest = dict(version=version, component='ble-console-source',
                    firmwareCompatibility=f'{version} / protocol 1',
                    requiredCapabilities=['account-v2', 'selected-execution'],
                    sourceCommit=source_commit, sourceDirty=source_dirty,
                    files=[dict(name=name, size=len(data), sha256=digest(data)) for name, data in files.items()])
    files['manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode('utf-8')
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            archive.writestr(name, data)
    validate_archive(output, version, source_commit, source_dirty)
    return manifest


def validate_archive(path, version, source_commit, source_dirty=False):
    with zipfile.ZipFile(path) as archive:
        manifest = json.loads(archive.read('manifest.json'))
        assert manifest['version'] == version and manifest['component'] == 'ble-console-source', 'Wrong BLE component/version'
        assert manifest['sourceCommit'] == source_commit and manifest['sourceDirty'] is source_dirty, 'BLE source provenance mismatch'
        expected = set(SOURCES) | {'PACKAGE.md'}
        assert len(archive.namelist()) == len(expected) + 1, 'Unexpected or duplicate BLE ZIP entry'
        assert set(archive.namelist()) == expected | {'manifest.json'}, 'Missing or unexpected BLE source'
        entries = manifest['files']
        assert len(entries) == len(expected) and {f['name'] for f in entries} == expected, 'Invalid BLE file manifest'
        for entry in entries:
            data = archive.read(entry['name'])
            assert len(data) == entry['size'] and digest(data) == entry['sha256'], 'BLE source digest mismatch'
            if Path(entry['name']).suffix in {'.py', '.pyw'}:
                ast.parse(data.decode('utf-8-sig'), filename=entry['name'])
        assert archive.read('VERSION').decode().strip() == version, 'Wrong embedded BLE version'
        assert {'account-v2', 'selected-execution'} == set(manifest['requiredCapabilities']), 'Wrong BLE capability requirements'
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', default=(ROOT / 'VERSION').read_text().strip())
    args = parser.parse_args()
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT).strip())
    build_archive(args.output, args.version, commit, dirty)
    print(f'BLE console source ZIP verified: {args.output.name}')


if __name__ == '__main__':
    main()
