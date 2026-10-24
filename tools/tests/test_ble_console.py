import asyncio
import contextlib
import io
import json
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

bleak_stub = types.ModuleType("bleak")
bleak_stub.BleakClient = type("BleakClient", (), {})
bleak_stub.BleakScanner = type("BleakScanner", (), {})
with patch.dict(sys.modules, {"bleak": bleak_stub}):
    from ble_console import Console


class MockClient:
    def __init__(self):
        self.writes = []

    async def write_gatt_char(self, _uuid, data, response=False):
        self.writes.append(json.loads(data))


class TestWxCommands(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.client = MockClient()
        self.console = Console(self.client)
        self.output = io.StringIO()
        self.redirect = contextlib.redirect_stdout(self.output)
        self.redirect.__enter__()

    async def asyncTearDown(self):
        self.redirect.__exit__(None, None, None)

    async def test_wx_commands_build_expected_payloads_and_redact_credentials(self):
        await self.console.run_command(
            "wx import 2 openid-secret access-secret refresh-secret 1792890000"
        )
        await self.console.run_command("wx status")
        await self.console.run_command("wx test 2")

        self.assertEqual(
            [payload["op"] for payload in self.client.writes],
            ["wx.import", "wx.status", "wx.test"],
        )
        self.assertEqual(
            self.client.writes[0],
            {
                "op": "wx.import",
                "slot": 2,
                "openid": "openid-secret",
                "access": "access-secret",
                "refresh": "refresh-secret",
                "expiresAt": 1792890000,
                "id": 1,
            },
        )
        self.assertEqual(self.client.writes[1]["id"], 2)
        self.assertEqual(self.client.writes[2], {"op": "wx.test", "slot": 2, "id": 3})
        for secret in ("openid-secret", "access-secret", "refresh-secret"):
            self.assertNotIn(secret, self.output.getvalue())

    async def test_invalid_wx_parameters_are_not_sent(self):
        await self.console.run_command("wx test 6")
        await self.console.run_command("wx import x openid access refresh 100")
        await self.console.run_command("wx import 0 openid access refresh not-a-timestamp")

        self.assertEqual(self.client.writes, [])

    async def test_raw_command_requires_a_json_object(self):
        await self.console.run_command("raw []")

        self.assertEqual(self.client.writes, [])


if __name__ == "__main__":
    unittest.main()
