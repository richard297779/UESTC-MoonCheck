import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from board_targets import ROOT, catalog, partitions, validate_image, validate_partition_binary


class BoardTargetsTest(unittest.TestCase):
    def test_catalog_has_six_targets_and_only_baselines_default(self):
        boards = catalog()
        self.assertEqual(6, len(boards))
        self.assertEqual({"s3-n16r8", "wroom32-4mb"}, {b["id"] for b in boards if b["defaultRelease"]})
        self.assertTrue(all("Not physically validated" in b["physicalValidation"] for b in boards if b["status"] == "experimental"))

    def test_duplicate_and_experimental_default_are_rejected(self):
        original = json.loads((ROOT / "firmware/board-targets.json").read_text())
        for mutation in ("duplicate", "default", "pin", "schema"):
            data = copy.deepcopy(original)
            if mutation == "duplicate": data["boards"].append(data["boards"][0])
            if mutation == "default": data["boards"] = [dict(data["boards"][2], defaultRelease=True)]
            if mutation == "pin": data["boards"] = [dict(data["boards"][-1], ledPin=48)]
            if mutation == "schema": data["schemaVersion"] = 999
            with tempfile.TemporaryDirectory() as folder:
                root = Path(folder); (root / "firmware").mkdir()
                (root / "firmware/board-targets.json").write_text(json.dumps(data))
                with self.assertRaises(AssertionError): catalog(root)

    def test_image_chip_mismatch_rejected(self):
        image = bytearray(24); image[0] = 0xE9; struct.pack_into("<H", image, 12, 5)
        validate_image(image, {"chip": "esp32c3"})
        with self.assertRaises(AssertionError): validate_image(image, {"chip": "esp32"})
        with self.assertRaises(AssertionError): validate_image(b"old file", {"chip": "esp32c3"})

    def test_partition_binary_mismatch_rejected(self):
        parts = partitions("nvs,data,nvs,0x9000,0x5000,\n")
        binary = struct.pack("<HBBII16sI", 0x50AA, 1, 2, 0x9000, 0x5000, b"nvs", 0)
        validate_partition_binary(binary, parts)
        with self.assertRaises(AssertionError): validate_partition_binary(binary, [dict(parts[0], size=1)])

    def test_partition_layout_fits_flash_and_retains_nvs(self):
        for board in catalog():
            parts = partitions((ROOT / "firmware" / board["partitionFile"]).read_text())
            self.assertEqual((0x9000, 0x5000), next((p["offset"], p["size"]) for p in parts if p["name"] == "nvs"))
            self.assertEqual(2, len([p for p in parts if p["type"] == "app"]))


if __name__ == "__main__":
    unittest.main()
