"""SCons post script: capture actual resolved board and upload configuration."""
import hashlib
import json
from pathlib import Path

Import("env")


def capture(source, target, env):
    root = Path(env.subst("$PROJECT_DIR"))
    board = env.BoardConfig()
    flags = env.ParseFlags(env.get("BUILD_FLAGS", []))["CPPDEFINES"]
    defines = {x[0]: x[1] for x in flags if isinstance(x, (tuple, list))}
    board_id = str(defines.get("WANAN_BOARD_ID", "custom-unverified")).replace('\\"', '').strip('"')
    psram = "none"
    if any(x == "BOARD_HAS_PSRAM" or isinstance(x, (tuple, list)) and x[0] == "BOARD_HAS_PSRAM" for x in flags):
        psram = "opi-8mb" if board.get("build.arduino.memory_type", "").endswith("_opi") else "qspi-8mb"
    offsets = {Path(env.subst(str(path))).name: str(offset) for offset, path in env.get("FLASH_EXTRA_IMAGES", [])}
    offsets["firmware.bin"] = env.subst("$ESP32_APP_OFFSET")
    partition = root / board.get("build.partitions")
    meta = dict(schemaVersion=1, env=env.subst("$PIOENV"), boardId=board_id,
        chip=board.get("build.mcu"), flashSize=board.get("upload.flash_size"),
        flashMode=env.subst("$BOARD_FLASH_MODE"), flashFreq=str(int(board.get("build.f_flash", "40000000L").rstrip("L")) // 1000000) + "m",
        psram=psram, ledPin=int(defines.get("WANAN_LED_PIN", -1)), ledRgb=str(defines.get("WANAN_LED_IS_RGB", 0)) == "1",
        partitionSha256=hashlib.sha256(partition.read_bytes()).hexdigest(), offsets=offsets)
    (Path(env.subst("$BUILD_DIR")) / "board-build.json").write_text(json.dumps(meta, indent=2) + "\n")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", capture)
