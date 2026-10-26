import asyncio
import base64
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
        self.mtu_size = 512

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

    async def test_raw_command_preserves_repeated_spaces(self):
        await self.console.run_command('raw {"op":"wifi.set","ssid":"room  wifi","pass":" a  b "}')
        self.assertEqual(self.client.writes[0]["ssid"], "room  wifi")
        self.assertEqual(self.client.writes[0]["pass"], " a  b ")

    async def test_all_sensitive_command_fields_are_redacted(self):
        await self.console.send({"op": "auth.set", "user": "private-user", "pass": "private-pass", "bfp": "private-bfp"})
        await self.console.send({"op": "notify.set", "url": "https://example.invalid/private-key"})
        for secret in ("private-user", "private-pass", "private-bfp", "private-key"):
            self.assertNotIn(secret, self.output.getvalue())

    async def test_utf8_is_decoded_after_reassembly(self):
        frame = json.dumps({"id": 1, "msg": "中文结果"}, ensure_ascii=False).encode()
        cut = next(i for i, byte in enumerate(frame) if byte >= 128) + 1
        for seq, piece in enumerate((frame[:cut], frame[cut:])):
            self.console._on_notify(None, json.dumps({"seq": seq, "of": 2, "b64": base64.b64encode(piece).decode()}).encode())
        self.assertIn("中文结果", self.output.getvalue())
        self.assertNotIn("\ufffd", self.output.getvalue())

    async def test_long_commands_are_written_as_mtu_sized_bytes(self):
        class PacketClient:
            mtu_size = 60
            def __init__(self): self.packets = []
            async def write_gatt_char(self, uuid, data, response=False):
                self.packets.append(data)
                self.response = response
        client = PacketClient()
        console = Console(client)
        await console.send({"op": "wx.import", "access": "测试" * 80})
        self.assertTrue(client.response)
        self.assertTrue(all(len(packet) <= 57 for packet in client.packets))
        self.assertEqual(json.loads(b"".join(client.packets))["access"], "测试" * 80)
        self.assertEqual(b"".join(client.packets).count(b"\n"), 1)

    async def test_oversized_command_never_reaches_transport(self):
        with self.assertRaises(ValueError):
            await self.console.send({"op": "raw", "body": "x" * 4096})
        self.assertEqual(self.client.writes, [])

    async def test_malformed_chunks_are_discarded_without_crashing(self):
        for message in ({"seq": -1, "of": 2, "b64": "abc"},
                        {"seq": 0, "of": 999999, "b64": ""},
                        {"seq": 0, "of": 1, "b64": "invalid!"}):
            self.console._on_notify(None, json.dumps(message).encode())
        self.assertEqual(self.console.pending_chunks, {})
        self.assertIn("无效", self.output.getvalue())


if __name__ == "__main__":
    unittest.main()
