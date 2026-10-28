"""Package independently versioned desktop and BLE sources; no upload or account access."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    version = (ROOT / 'desktop/VERSION').read_text().strip()
    tag = 'desktop-v' + version
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    assert not subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT).strip(), 'Commit all source changes first'
    output = ROOT / 'desktop/dist/release' / tag
    output.mkdir(parents=True, exist_ok=True)
    desktop = ROOT / 'desktop/dist/packages' / ('MoonCheck-Desktop-' + version + '-windows.zip')
    with zipfile.ZipFile(desktop) as archive:
        manifest = json.loads(archive.read('manifest.json'))
        assert manifest['sourceCommit'] == commit and manifest['sourceDirty'] is False, 'Repackage desktop against clean HEAD'
        assert set(archive.namelist()) == {f['name'] for f in manifest['files']} | {'manifest.json'}
        for entry in manifest['files']:
            data = archive.read(entry['name'])
            assert len(data) == entry['size'] and digest(data) == entry['sha256'], 'Desktop ZIP digest mismatch'
        for name, expected in manifest['sourceFiles'].items():
            assert digest((ROOT / 'desktop' / name).read_bytes()) == expected, 'Desktop source differs from executable build'
    shutil.copyfile(desktop, output / desktop.name)
    ble_sources = ['tools/ble_console.py', 'tools/ble_console.pyw', 'tools/ble_gui.py', 'tools/requirements-ble.txt']
    files = {name: (ROOT / name).read_bytes() for name in ble_sources}
    files['LICENSE'] = (ROOT / 'LICENSE').read_bytes()
    files['PACKAGE.md'] = f'''# MoonCheck 电脑 BLE 图形控制台（配套 Desktop {version}）

此工具通过 BLE 控制 ESP32，与独立运行的 MoonCheck.exe 是两个程序。
需安装 Python 3.10+（含 tkinter），在解压根目录执行：

    python -m pip install -r tools/requirements-ble.txt

然后双击 tools/ble_console.pyw（关联 Python 的 pythonw.exe），或执行：

    python tools/ble_console.py --gui

扫描后核对地址并连接。设备概览、账号列表和设置默认值由设备查询获取；完整回执和 JSON 默认收于开发者选项。
密码与令牌不回填，不持久化日志。真实打卡、通知、调度等操作须谨慎确认。GUI BLE 实机联调尚待验证。
建议配合固件 0.7.0；不要把固件/APK 的用户验收结论沿用到此 GUI。
说明：https://github.com/richard297779/UESTC-MoonCheck/blob/{tag}/docs/ble-gui.md
SHA-256 与源码来源见 manifest.json。依赖 bleak 通过 pip 单独安装，本包不包含第三方库或本机配置。
'''.encode()
    files['manifest.json'] = (json.dumps(dict(version=version, component='ble-console-source',
        firmwareCompatibility='0.7.0 / protocol 1', sourceCommit=commit, sourceDirty=False,
        files=[dict(name=name, size=len(data), sha256=digest(data)) for name, data in files.items()]), indent=2) + '\n').encode()
    ble = output / ('MoonCheck-BLE-Console-' + version + '-source.zip')
    with zipfile.ZipFile(ble, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in files.items():
            archive.writestr(name, data)
    assets = [output / desktop.name, ble]
    release_manifest = dict(version=version, tag=tag, sourceCommit=commit, sourceDirty=False,
        kind='prerelease', firmwareVersion=(ROOT / 'VERSION').read_text().strip(),
        validation='Offline tests and Windows frozen smoke only; no live desktop account or BLE GUI validation; no Linux binary',
        files=[dict(name=p.name, size=p.stat().st_size, sha256=digest(p.read_bytes())) for p in assets])
    manifest_file = output / 'computer-release-manifest.json'
    manifest_file.write_text(json.dumps(release_manifest, indent=2) + '\n', encoding='utf-8')
    assets.append(manifest_file)
    (output / 'sha256.txt').write_text(''.join(digest(p.read_bytes()) + '  ' + p.name + '\n' for p in assets), encoding='ascii')
    print('Computer release packaged:', tag, '4 assets; provenance and hashes verified')


if __name__ == '__main__':
    main()
