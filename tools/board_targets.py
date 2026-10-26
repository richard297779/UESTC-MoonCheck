"""Board catalog, partition and image validation shared by build/release checks."""
import csv
import hashlib
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHIP_IDS = {"esp32": 0, "esp32s3": 9, "esp32c3": 5}


def catalog(root=ROOT):
    data = json.loads((root / "firmware/board-targets.json").read_text())
    assert data["schemaVersion"] == 1, "Unknown board catalog schema"
    boards = data["boards"]
    assert len({b["id"] for b in boards}) == len(boards), "Duplicate board ID"
    assert len({b["env"] for b in boards}) == len(boards), "Duplicate environment"
    for b in boards:
        assert b["chip"] in CHIP_IDS, "Unsupported chip"
        assert b["status"] in {"baseline", "experimental"}, "Unknown validation status"
        assert b["status"] != "experimental" or not b["defaultRelease"], "Experimental target cannot be a default release"
        assert b["ledPin"] == -1 or 0 <= b["ledPin"] <= (21 if b["chip"] == "esp32c3" else 48), "Invalid LED pin"
        partition_path = (root / "firmware" / b["partitionFile"]).resolve()
        assert partition_path.is_relative_to((root / "firmware/partitions").resolve()), "Invalid partition path"
        parts = partitions(partition_path.read_text(encoding="ascii"))
        limit = int(b["flashSize"].removesuffix("MB")) * 1024 * 1024
        assert all(p["offset"] + p["size"] <= limit for p in parts), "Partition exceeds flash"
        assert all(a["offset"] + a["size"] <= z["offset"] for a, z in zip(parts, parts[1:])), "Partition overlap"
        assert next(p for p in parts if p["name"] == "app0")["offset"] == int(b["appOffset"], 0), "Wrong app offset"
    return boards


def partitions(text):
    rows = []
    for row in csv.reader(line for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")):
        rows.append(dict(name=row[0].strip(), type=row[1].strip(), subtype=row[2].strip(), offset=int(row[3].strip(), 0), size=int(row[4].strip(), 0)))
    return sorted(rows, key=lambda p: p["offset"])


def validate_image(data, board):
    assert len(data) >= 24 and data[0] == 0xE9, "Invalid ESP image header"
    assert struct.unpack_from("<H", data, 12)[0] == CHIP_IDS[board["chip"]], "Wrong image chip"


def validate_partition_binary(data, parts):
    actual = []
    for start in range(0, len(data) - 31, 32):
        magic, kind, subtype, offset, size, label, flags = struct.unpack_from("<HBBII16sI", data, start)
        if magic != 0x50AA:
            break
        actual.append((label.split(b"\0")[0].decode(), kind, subtype, offset, size))
    expected = [(p["name"], 0 if p["type"] == "app" else 1,
                 {"nvs": 2, "ota": 0, "ota_0": 16, "ota_1": 17, "spiffs": 130, "coredump": 3}[p["subtype"]], p["offset"], p["size"]) for p in parts]
    assert actual == expected, "Partition binary differs from catalog CSV"


def validate_build(board, build, root=ROOT):
    meta = json.loads((build / "board-build.json").read_text())
    assert meta["schemaVersion"] == 1 and meta["env"] == board["env"], "Wrong build metadata"
    assert meta["boardId"] == board["id"] and meta["chip"] == board["chip"], "Wrong board identity"
    for key in ("flashSize", "flashMode", "flashFreq", "psram", "ledPin", "ledRgb"):
        assert meta[key] == board[key], f"Build/catalog mismatch: {key}"
    partition = root / "firmware" / board["partitionFile"]
    assert meta["partitionSha256"] == hashlib.sha256(partition.read_bytes()).hexdigest(), "Stale partition metadata"
    expected = {"bootloader.bin": board["bootOffset"], "partitions.bin": board["partitionOffset"], "boot_app0.bin": board["bootAppOffset"], "firmware.bin": board["appOffset"]}
    assert set(meta["offsets"]) == set(expected), "Unexpected flash files"
    for name, offset in expected.items():
        assert int(meta["offsets"][name], 0) == int(offset, 0), f"Wrong flash offset: {name}"
    app = (build / "firmware.bin").read_bytes()
    validate_image(app, board)
    for part in partitions(partition.read_text(encoding="ascii")):
        if part["type"] == "app":
            assert len(app) <= part["size"], "App exceeds OTA slot"
    validate_partition_binary((build / "partitions.bin").read_bytes(), partitions(partition.read_text(encoding="ascii")))
    return meta


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate-builds", action="store_true")
    parser.add_argument("--only", default="")
    parser.add_argument("--include-experimental", action="store_true")
    args = parser.parse_args()
    boards = catalog()
    if args.only:
        aliases = {"s3": "s3-n16r8", "wroom32": "wroom32-4mb"}
        selected = aliases.get(args.only, args.only)
        boards = [b for b in boards if b["id"] == selected]
        assert boards, "Unknown target"
    elif not args.include_experimental:
        boards = [b for b in boards if b["defaultRelease"]]
    if args.validate_builds:
        for board in boards:
            validate_build(board, ROOT / "firmware/.pio/build" / board["env"])
    print(json.dumps(boards))
