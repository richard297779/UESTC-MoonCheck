"""Offline source/ZIP/frozen-Python privacy guard. Reports locations, never values."""
import argparse
import io
import marshal
from pathlib import Path
import re
import subprocess
import sys
import types
import zipfile

ROOT = Path(__file__).resolve().parents[2]
RULES = {
    'private-key': rb'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----\r?\n[A-Za-z0-9+/=\r\n]{80,}-----END (?:RSA |EC |OPENSSH )?PRIVATE KEY-----',
    'github-token': rb'\b(?:gh[pousr]_[A-Za-z0-9_]{30,}|github_pat_[A-Za-z0-9_]{30,})',
    'private-notion-id': rb'https://(?:app\.)?notion\.(?:com|so)/(?:p/)?[0-9a-f]{32}\b',
    'machine-user-path': rb'(?i)[A-Z]:[\\/]+Users[\\/]+(?!\.\.\.|<|\[|example|your-user|username|user[\\/])[A-Za-z0-9_-]+[\\/]',
}
DENIED = {'config.json', 'local.properties', 'keystore.properties', 'debug.keystore'}


def scan_bytes(label, data, issues, depth=0):
    for name, pattern in RULES.items():
        if re.search(pattern, data) or re.search(pattern, data.replace(b'\0', b'')):
            issues.append((label, name))
    if data.startswith(b'PK\x03\x04') and depth < 5:
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            assert sum(e.file_size for e in archive.infolist()) < 512 * 1024 * 1024, 'Archive too large'
            for entry in archive.infolist():
                if entry.is_dir():
                    continue
                member = label + '!' + entry.filename
                if Path(entry.filename).name in DENIED or Path(entry.filename).suffix.lower() in {'.jks', '.keystore', '.har'}:
                    issues.append((member, 'private-file'))
                scan_bytes(member, archive.read(entry), issues, depth + 1)


def scan_frozen(executable, issues):
    from PyInstaller.archive.readers import CArchiveReader
    archive = CArchiveReader(str(executable))
    pyz = archive.open_embedded_archive('PYZ.pyz')
    for name in pyz.toc:
        code = pyz.extract(name)
        if isinstance(code, types.CodeType):
            scan_bytes('frozen:' + name, marshal.dumps(code), issues)
    for name, entry in archive.toc.items():
        if entry[-1] == 's':
            scan_bytes('entry:' + name, archive.extract(name), issues)
    print('Frozen Python archive checked:', len(pyz.toc), 'modules')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', action='store_true')
    parser.add_argument('--frozen', type=Path)
    parser.add_argument('--include-historical', action='store_true', help='Also block on immutable historical binary assets')
    parser.add_argument('artifacts', type=Path, nargs='*')
    args = parser.parse_args()
    issues = []
    if args.source:
        files = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=ROOT).decode().split('\0')
        historical = 0
        for name in set(files) - {''}:
            path = ROOT / name
            if path.is_file():
                if name.startswith('release/') and path.suffix in {'.bin', '.zip'} and not args.include_historical:
                    historical += 1
                    continue
                scan_bytes(name, path.read_bytes(), issues)
        print('Repository files checked:', len(set(files) - {''}))
        print('Historical binaries retained unchanged (not current artifacts):', historical)
    for artifact in args.artifacts:
        scan_bytes(artifact.name, artifact.read_bytes(), issues)
    if args.frozen:
        scan_frozen(args.frozen, issues)
    for label, rule in issues:
        print('BLOCKED:', label, rule)
    print('Privacy guard:', len(issues), 'findings')
    return int(bool(issues))


if __name__ == '__main__':
    sys.exit(main())
