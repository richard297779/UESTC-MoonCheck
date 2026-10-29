"""Package independently versioned desktop and BLE sources; no upload or account access."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

from package_ble_console import build_archive

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
    ble_version = (ROOT / 'VERSION').read_text().strip()
    ble = output / f'MoonCheck-{ble_version}-ble-console-source.zip'
    build_archive(ble, ble_version, commit, False)
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
