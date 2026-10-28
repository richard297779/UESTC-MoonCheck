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
from board_targets import catalog, partitions, validate_image, validate_partition_binary

ROOT = Path(__file__).resolve().parents[1]


def check_sources():
    issues = []
    release_root = ROOT / "release"
    if release_root.exists():
        for entry in release_root.iterdir():
            if entry.name == "README.md" and entry.is_file():
                continue
            if entry.is_dir() and (entry.name == ".staging" or re.fullmatch(r"v\d+\.\d+\.\d+", entry.name)):
                continue
            issues.append(f"Unversioned release entry: {entry.name}; use release/vX.Y.Z/ or .staging/")
    try:
        catalog()
    except (AssertionError, ValueError, KeyError, OSError) as error:
        issues.append(f"Board catalog: {error}")
    version = (ROOT / "VERSION").read_text().strip()
    header = (ROOT / "firmware/include/app_version.h").read_text(encoding="utf-8")
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        issues.append("VERSION must be X.Y.Z")
    if f'#define FW_VERSION "{version}"' not in header:
        issues.append("firmware version differs from VERSION")
    gradle = (ROOT / "android/app/build.gradle.kts").read_text(encoding="utf-8")
    if 'versionName = rootProject.file("../VERSION").readText().trim()' not in gradle:
        issues.append("Android versionName must read VERSION")
    desktop_version = (ROOT / "desktop/VERSION").read_text().strip()
    desktop_init = (ROOT / "desktop/src/mooncheck_app/__init__.py").read_text(encoding="utf-8")
    desktop_pe = (ROOT / "desktop/version_info.txt").read_text(encoding="utf-8")
    if f"__version__ = '{desktop_version}'" not in desktop_init or f"StringStruct('FileVersion','{desktop_version}')" not in desktop_pe:
        issues.append("Desktop VERSION, Python and Windows file version differ")
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
    assert manifest.get("schemaVersion", 1) in {1, 2}, "Unknown manifest schema"
    version = (ROOT / "VERSION").read_text().strip()
    assert manifest["version"] == version, "Package version differs from source"
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    assert manifest.get("documentationCommit", manifest["sourceCommit"]) == head, "Package provenance differs from HEAD"
    if manifest["sourceCommit"] != head:
        assert subprocess.run(["git", "merge-base", "--is-ancestor", manifest["sourceCommit"], head], cwd=ROOT).returncode == 0, "Binary source commit is not an ancestor"
        changed = subprocess.check_output(["git", "diff", "--name-only", manifest["sourceCommit"], head], cwd=ROOT, text=True).splitlines()
        assert all(name.endswith(".md") or name in {"dev/tools/make_release.ps1", "tools/check_project.py"} for name in changed), "Runtime code changed after binary build"
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
    guide_ref = manifest.get("documentationCommit", f"v{version}")
    assert f"https://github.com/{manifest['repository']}/blob/{guide_ref}/docs/quickstart.md" in guide, "Package guide link is broken"
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
            if manifest.get("schemaVersion", 1) == 1:
                assert merged[0x10000:] == app, "Merged app differs from full ZIP"
                assert merged[0x9000:0xE000] == b"\xff" * 0x5000, "Merged NVS gap is unexpected"
            else:
                profiles = manifest["boardProfiles"]
                assert len(profiles) == len(manifest["boards"]) and {p["id"] for p in profiles} == set(manifest["boards"]), "Invalid board profiles"
                profile = next(p for p in profiles if p["id"] == board)
                target = next((b for b in catalog() if b["id"] == board), None)
                assert target is not None, "Unknown release board"
                for field in ("env", "chip", "flashSize", "psram", "flashMode", "flashFreq", "status"):
                    assert profile[field] == target[field], f"Release profile differs from catalog: {field}"
                assert profile["status"] in {"baseline", "experimental"}, "Invalid validation status"
                assert "pending" in profile["physicalValidation"] or "Not physically validated" in profile["physicalValidation"], "Unverified physical validation claim"
                validate_image(app, profile)
                part_text = profile["partitions"]
                # PowerShell may preserve CRLF; hash the exact UTF-8 partition source.
                assert digest(part_text.encode()) == profile["partitionSha256"], "Partition source digest mismatch"
                parts = partitions(part_text)
                validate_partition_binary(archive.read("partitions.bin"), parts)
                offsets = profile["offsets"]
                assert set(offsets) == {"bootloader.bin", "partitions.bin", "boot_app0.bin", "firmware.bin"}, "Unexpected flash images"
                for file, key in (("bootloader.bin", "bootOffset"), ("partitions.bin", "partitionOffset"), ("boot_app0.bin", "bootAppOffset"), ("firmware.bin", "appOffset")):
                    assert int(offsets[file], 0) == int(target[key], 0), "Release flash offset differs from catalog"
                app_offset = int(offsets["firmware.bin"], 0)
                assert merged[app_offset:] == app, "Merged app differs from full ZIP"
                limit = int(profile["flashSize"].removesuffix("MB")) * 1024 * 1024
                assert len(merged) <= limit and all(p["offset"] + p["size"] <= limit for p in parts), "Image/partition exceeds flash"
                for p in parts:
                    if p["type"] == "app":
                        assert len(app) <= p["size"], "App exceeds OTA slot"
                    if p["subtype"] == "nvs":
                        assert merged[p["offset"]:p["offset"] + p["size"]] == b"\xff" * p["size"], "Unexpected NVS data"
                for file, offset in offsets.items():
                    binary = archive.read(file)
                    begin = int(offset, 0)
                    actual = merged[begin:begin + len(binary)]
                    if file == "bootloader.bin":
                        validate_image(binary, profile)
                        assert actual[:2] == binary[:2] and actual[4:] == binary[4:], "Merged bootloader differs"
                    else:
                        assert actual == binary, f"Merged {file} differs"
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
