"""Package an existing Windows desktop build; never uploads or reads live config."""
import hashlib
import importlib.metadata
import json
from pathlib import Path
import subprocess
import sys
import os
import tempfile
import zipfile

BASE = Path(__file__).resolve().parent
ROOT = BASE.parent


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    import pefile
    version = (BASE / 'VERSION').read_text().strip()
    numbers = tuple(map(int,version.split('.'))) + (0,)
    executable = BASE / 'dist/windows/MoonCheck.exe'
    pe = pefile.PE(str(executable),fast_load=False)
    v = pe.VS_FIXEDFILEINFO[0]
    actual = (v.FileVersionMS >> 16,v.FileVersionMS & 65535,v.FileVersionLS >> 16,v.FileVersionLS & 65535)
    pe.close()
    assert actual == numbers,'Windows executable version differs from desktop/VERSION'
    sources = list((BASE/'src').rglob('*.py')) + [BASE/'mooncheck.py',BASE/'mooncheck.spec',BASE/'VERSION',BASE/'version_info.txt']
    assert all(p.stat().st_mtime <= executable.stat().st_mtime for p in sources),'Executable is older than its sources'
    sys.path.insert(0,str(BASE/'src'))
    from mooncheck_app.config import default_config
    build = BASE/'build';build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='smoke-',dir=build) as folder:
        assert Path(folder).resolve().is_relative_to(build.resolve())
        cfg = default_config();cfg.update(student_id='fixture-student',password='fixture-password')
        config_path = Path(folder)/'config.json'
        config_path.write_text(json.dumps(cfg,ensure_ascii=False),encoding='utf-8')
        environment = os.environ.copy();environment['MOONCHECK_CONFIG'] = str(config_path)
        environment['TEMP'] = environment['TMP'] = folder
        for args in (['--version'],['selftest'],['checkin','--dry-run'],['wx','status']):
            result = subprocess.run([str(executable)]+args,env=environment,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=40)
            assert result.returncode == 0,'Frozen smoke command failed: '+str(args)+' '+ascii(result.stderr[:300])
            if args == ['--version']:assert version.encode() in result.stdout
            if args == ['selftest']:assert b'PASS' in result.stdout
    print('Frozen smoke: version, crypto selftest, dry-run and authorization status passed')
    guide = '''# MoonCheck Desktop {version} (Windows)

这是桌面端本地测试构建，未完成真实桌面账号联调；固件/Android 的确认不代表桌面已实测。

1. 解压整个目录至本人可写、受保护的位置，双击 MoonCheck.exe。无需安装 Python。
2. 基本页填写 CAS 学号、密码、本人浏览器指纹和现场坐标；保存配置。
3. 多人先新增账号并保存；微信授权页选择所属账号，填写本人 openid/access/refresh。到期 Unix 秒未知可填 0，导入立即保存。
4. 初次微信授权仍由本人通过 QR Connect 准备。不要同时在固件、另一台电脑或授权网页刷新同一套授权。
5. 先取票测试（不提交），再登录、查状态；允许窗口内真实打卡并复查。自动化由系统调度器调用 MoonCheck.exe auto，程序不会创建系统任务。
6. config.json 含明文凭据，不要共享；界面导出为递归脱敏副本。与 config.json 同目录的 .lock 文件由程序管理，不必手动删除。

CLI: MoonCheck.exe --version / wx import / wx status / wx test / checkin --dry-run / auto。
多账号授权使用 wx import -a A01 / wx test -a A01，不能借用其他账号的授权。
同一配置有进程互斥，窗口使用 UTC+8，票据失败或结果未知不自动重放提交。
SHA-256 与来源信息见 manifest.json、sha256.txt；许可见 LICENSE、THIRD_PARTY.md 和 licenses/。
'''.format(version=version)
    files = {'MoonCheck.exe':executable.read_bytes(), 'PACKAGE.md':guide.encode(),
             'config.example.json':(BASE/'config.example.json').read_bytes(),
             'THIRD_PARTY.md':(BASE/'THIRD_PARTY.md').read_bytes(),'LICENSE':(ROOT/'LICENSE').read_bytes()}
    dependencies = {}
    for name in ('pycryptodome','pyinstaller'):
        distribution = importlib.metadata.distribution(name)
        dependencies[name] = distribution.version
        for entry in distribution.files or []:
            if entry.name.lower().startswith(('license','copying')):
                path = Path(distribution.locate_file(entry))
                if path.is_file():files['licenses/'+name+'/'+str(entry).replace('\\','/').replace('../','')] = path.read_bytes()
    for filename in ('LICENSE.txt','LICENSE_PYTHON.txt'):
        path = Path(sys.prefix)/filename
        if path.is_file():files['licenses/python/'+filename] = path.read_bytes()
    for base in (Path(sys.prefix)/'tcl',Path(sys.prefix)/'Library/lib'):
        for pattern in ('tcl*/license.terms','tk*/license.terms'):
            for path in base.glob(pattern):
                files['licenses/'+path.parent.name+'/license.terms'] = path.read_bytes()
    manifest = dict(version=version,platform='windows-x86_64',
        sourceCommit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
        sourceDirty=bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True).strip()),
        validation='Offline tests and frozen executable smoke tests; no live desktop account validation',
        dependencies=dependencies,python=sys.version.split()[0],
        sourceFiles={str(p.relative_to(BASE)).replace('\\','/'):sha(p.read_bytes()) for p in sources},
        files=[dict(name=name,size=len(data),sha256=sha(data)) for name,data in files.items()])
    files['manifest.json'] = (json.dumps(manifest,ensure_ascii=False,indent=2)+'\n').encode()
    output = BASE/'dist/packages';output.mkdir(parents=True,exist_ok=True)
    archive = output/('MoonCheck-Desktop-'+version+'-windows.zip')
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
        for name,data in files.items():z.writestr(name,data)
    with zipfile.ZipFile(archive) as z:
        assert set(z.namelist()) == set(files)
        for name,data in files.items():assert z.read(name)==data
    (output/'manifest.json').write_bytes(files['manifest.json'])
    hashes = sha(archive.read_bytes())+'  '+archive.name+'\n'+sha(files['MoonCheck.exe'])+'  ../windows/MoonCheck.exe\n'
    (output/'sha256.txt').write_text(hashes,encoding='ascii')
    print('Packaged',archive.name,'with',len(files),'files; version, source freshness and ZIP content verified')


if __name__=='__main__':main()
