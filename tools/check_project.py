#!/usr/bin/env python3
"""Offline release checks: versions, Python syntax, local doc links and package integrity."""
import argparse
import ast
import hashlib
import json
import re
import subprocess
import sys
import zipfile
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]


def check_sources():
    issues = []
    version = (ROOT / "VERSION").read_text().strip()
    header = (ROOT / "firmware/include/app_version.h").read_text(encoding="utf-8")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        issues.append("VERSION must be X.Y.Z")
    if f'#define FW_VERSION "{version}"' not in header:
        issues.append("firmware version differs from VERSION")
    gradle = (ROOT / "android/app/build.gradle.kts").read_text(encoding="utf-8")
    if 'versionName = rootProject.file("../VERSION").readText().trim()' not in gradle:
        issues.append("Android versionName must read VERSION")
    files = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT).decode().split("\0")
    python_count = link_count = 0
    for name in sorted(set(files) - {""}):
        path = ROOT / name
        if not path.is_file():
            continue
        if path.suffix == ".py":
            try:
                ast.parse(path.read_text(encoding="utf-8-sig"), filename=name)
                python_count += 1
            except (SyntaxError, UnicodeError) as error:
                issues.append(f"Python syntax: {name}: {error}")
        if path.name == "config.json" or path.suffix in {".har", ".jks", ".keystore"} or path.name == "local.properties":
            issues.append(f"Local credentials or machine state would be committed: {name}")
        if path.suffix != ".md":
            continue
        text = re.sub(r"```.*?```", "", path.read_text(encoding="utf-8-sig"), flags=re.S)
        for target in re.findall(r"!?\[[^\]]*\]\(([^)]+)\)", text):
            target = target.strip().split(' "', 1)[0].strip("<>")
            if not target or re.match(r"[A-Za-z][A-Za-z0-9+.-]*:", target) or target.startswith("#"):
                continue
            relative = unquote(target.split("#", 1)[0].split("?", 1)[0])
            if not relative:
                continue
            link_count += 1
            if not (path.parent / relative).exists():
                issues.append(f"Broken local link: {name} -> {relative}")
    print(f"Source checks: version {version}, {python_count} Python files, {link_count} local links")
    return issues


def digest(data):
    return hashlib.sha256(data).hexdigest()


def check_package(directory, require_clean=False):
    directory = Path(directory).resolve()
    manifest = json.loads((directory / "release-manifest.json").read_text(encoding="utf-8-sig"))
    version = (ROOT / "VERSION").read_text().strip()
    assert manifest["version"] == version, "Package version differs from source"
    assert manifest["sourceCommit"] == subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(), "Package source commit differs from HEAD"
    if require_clean:
        assert manifest["sourceDirty"] is False, "Release was packaged from a dirty source tree"
        assert not subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True).strip(), "Working tree is not clean"
    expected = {}
    for entry in manifest["files"]:
        name = entry["name"]
        assert Path(name).name == name and name not in expected, "Invalid or duplicate asset name"
        data = (directory / name).read_bytes()
        assert len(data) == entry["size"] and digest(data) == entry["sha256"], f"Bad manifest digest: {name}"
        expected[name] = data
    guide = expected["PACKAGE.md"].decode("utf-8-sig")
    assert f"https://github.com/{manifest['repository']}/blob/v{version}/docs/quickstart.md" in guide, "Package guide link is broken"
    if manifest["android"] is not None:
        assert manifest["android"]["version"] == version and manifest["android"]["package"] == "io.mooncheck.app", "Wrong APK metadata"
        apk = directory / f"MoonCheck-{version}-android-debug.apk"
        with zipfile.ZipFile(apk) as archive:
            assert {"AndroidManifest.xml", "classes.dex"}.issubset(archive.namelist()), "APK is missing its manifest or executable code"
    for board in manifest["boards"]:
        name = f"wanan-{version}-{board}"
        with zipfile.ZipFile(directory / f"{name}-full.zip") as archive:
            assert set(archive.namelist()) == {"bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin", "FLASH.txt"}, "Unexpected firmware ZIP entries"
            app = archive.read("firmware.bin")
            assert version.encode() in app, "Firmware version missing"
            merged = (directory / f"{name}-merged.bin").read_bytes()
            assert merged[0x10000:] == app, "Merged app differs from full ZIP"
            assert merged[0x9000:0xE000] == b"\xff" * 0x5000, "Merged NVS gap is unexpected"
    bundle_name = f"MoonCheck-{version}-release.zip"
    manifest_name = "release-manifest.json"
    expected[manifest_name] = (directory / manifest_name).read_bytes()
    with zipfile.ZipFile(directory / bundle_name) as bundle:
        assert set(bundle.namelist()) == set(expected), "Bundle contents differ from manifest"
        for name, data in expected.items():
            assert bundle.read(name) == data, f"Bundle entry mismatch: {name}"
    expected[bundle_name] = (directory / bundle_name).read_bytes()
    hashes = {}
    for line in (directory / "sha256.txt").read_text().splitlines():
        checksum, name = line.split("  ", 1)
        assert name not in hashes and name in expected, "Unexpected or duplicate checksum entry"
        hashes[name] = checksum
    assert set(hashes) == set(expected), "Missing checksum entries"
    for name, data in expected.items():
        assert digest(data) == hashes[name], f"SHA-256 mismatch: {name}"
    print(f"Package checks: {len(expected)} assets, exact bundle contents, hashes, firmware offsets and source commit OK")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-dir")
    parser.add_argument("--require-clean", action="store_true")
    args = parser.parse_args()
    issues = check_sources()
    for issue in issues:
        print("ERROR:", issue)
    if args.release_dir:
        try:
            check_package(args.release_dir, args.require_clean)
        except (AssertionError, OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
            issues.append(str(error))
            print("ERROR:", error)
    return bool(issues)


if __name__ == "__main__":
    sys.exit(main())
