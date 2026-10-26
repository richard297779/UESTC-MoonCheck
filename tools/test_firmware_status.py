"""Execute the production status builder with ArduinoJson and host ESP fixtures.

Requires a native C++17 compiler, or --zig pointing to a portable zig executable.
No board, BLE, network or real credentials are accessed by the test.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", type=Path)
    parser.add_argument("--compiler", default="c++")
    parser.add_argument("--arduinojson", type=Path, default=ROOT / "firmware/.pio/libdeps/esp32-s3-devkitc-1/ArduinoJson/src")
    args = parser.parse_args()
    source = (ROOT / "firmware/src/command_router.cpp").read_text(encoding="utf-8")
    start = source.index("String CommandRouter::buildStatusJson() const {")
    end = source.index("\n}\n", start) + 3
    production = source[start:end]
    # Reproduce the actual v0.7.0 defect with the SAME library. String writers
    # clear the existing destination, including the fw/chip/runtime prefix.
    legacy = production.replace("CommandRouter::buildStatusJson", "CommandRouter::legacyStatusJson")
    legacy = legacy.replace("String runtimeJson;\n  serializeJson(runtime, runtimeJson);\n  out += runtimeJson;", "serializeJson(runtime, out);")
    assert legacy != production.replace("CommandRouter::buildStatusJson", "CommandRouter::legacyStatusJson"), "Regression reproducer no longer matches source"
    main_source = r'''
#include <iostream>
int main() {
  CommandRouter router;
  TimeFixture time; NetworkFixture network; ConfigFixture config;
  CookieFixture cookies; AuthFixture auth; SchedulerFixture scheduler;
  for (int fixture = 0; fixture < 3; ++fixture) {
    if (fixture) {
      router.m_time = &time; router.m_wifi = &network; router.m_config = &config;
      router.m_cookies = &cookies; router.m_auth = &auth; router.m_scheduler = &scheduler;
    }
    WiFi.connected = fixture == 1;
    ESP.psram = fixture != 2;
    std::string reply = "{\"id\":2,\"evt\":\"ok\",\"data\":" + router.buildStatusJson() + "}";
    JsonDocument parsed;
    if (deserializeJson(parsed, reply)) return 10 + fixture;
    if (!parsed["data"]["runtime"].is<JsonObject>() || !parsed["data"]["net"].is<JsonObject>() ||
        !parsed["data"]["cfg"].is<JsonObject>() || !parsed["data"]["chip"].is<JsonObject>()) return 20 + fixture;
    std::string broken = "{\"id\":2,\"evt\":\"ok\",\"data\":" + router.legacyStatusJson() + "}";
    auto legacyError = deserializeJson(parsed, broken);
    // ArduinoJson accepts a valid prefix with trailing data; validate structure
    // too. Android's strict parser rejects the extra closing brace outright.
    if (!legacyError && parsed["data"]["runtime"].is<JsonObject>() && parsed["data"]["net"].is<JsonObject>()) return 30 + fixture;
    std::cout << reply << '\n' << broken << '\n';
  }
}
'''
    folder = ROOT / "android/.build-tools/status-json-test"
    folder.mkdir(parents=True, exist_ok=True)
    cpp = folder / "status_test.cpp"
    cpp.write_text('#include "native_shim.h"\n' + production + legacy + main_source, encoding="utf-8")
    executable = folder / "status_test.exe"
    command = [str(args.zig.resolve()), "c++"] if args.zig else [args.compiler]
    command += ["-std=c++17", "-O0", "-I" + str(args.arduinojson.resolve()),
                "-I" + str(ROOT / "firmware/tests/status_json"), "-I" + str(ROOT / "firmware/include"), str(cpp), "-o", str(executable)]
    environment = os.environ.copy()
    temporary = folder / "temp"
    temporary.mkdir(exist_ok=True)
    environment["TEMP"] = environment["TMP"] = str(temporary)
    environment["ZIG_GLOBAL_CACHE_DIR"] = str(folder / "zig-global-cache")
    environment["ZIG_LOCAL_CACHE_DIR"] = str(folder / "zig-local-cache")
    subprocess.run(command, check=True, cwd=ROOT, env=environment)
    output = subprocess.check_output([str(executable)], cwd=ROOT).decode("utf-8")
    lines = output.splitlines()
    assert len(lines) == 6
    fixtures = [json.loads(line) for line in lines[::2]]
    for broken in lines[1::2]:
        try:
            json.loads(broken)
        except json.JSONDecodeError:
            pass
        else:
            raise AssertionError("Legacy status unexpectedly became valid JSON")
    assert len(fixtures) == 3
    for reply in fixtures:
        data = reply["data"]
        assert data["fw"] == (ROOT / "VERSION").read_text().strip()
        assert data["runtime"]["heapPeakUsedBytes"] == 220000
        assert all(key in data for key in ("chip", "runtime", "net", "cfg", "cookies", "auth", "schedule", "last"))
    assert fixtures[1]["data"]["net"]["ssid"].startswith("宿舍 ") and '"' in fixtures[1]["data"]["net"]["ssid"]
    assert fixtures[2]["data"]["runtime"]["psramTotalBytes"] == 0
    # Reuse the actual production-generated reply for Android framing tests.
    resources = ROOT / "android/app/src/test/resources"
    resources.mkdir(exist_ok=True)
    (resources / "firmware-status.json").write_text(json.dumps(fixtures[1], ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("PASS: 3 production status fixtures; legacy bug reproduced/rejected; fixed replies parsed by ArduinoJson and Python")


if __name__ == "__main__":
    main()
