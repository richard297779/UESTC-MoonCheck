import asyncio
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ble_gui import Application, OPERATIONS, Session, build_payload, device_defaults, display_message


def spec(operation):
    return next(s for group in OPERATIONS.values() for s in group if s[1] == operation)


class Forms(unittest.TestCase):
    def test_defaults_come_from_device_without_secrets(self):
        snapshot = {"cfg": {"wifiSsid": "actual wifi", "user": "masked", "pass": "secret",
                            "schedule": {"checkinAt": "22:17", "autoDaily": True}}}
        self.assertEqual(device_defaults("wifi.set", snapshot), {"ssid": "actual wifi"})
        self.assertEqual(device_defaults("sched.set", snapshot), {"at": "22:17"})
        self.assertEqual(device_defaults("auto.set", snapshot), {"enabled": True})
        self.assertEqual(device_defaults("auth.set", snapshot), {})

    def test_account_projection_masks_even_unexpected_full_identifier(self):
        message = {"msg": "accounts", "data": [{"user": "1234567890", "pass": "private"}]}
        safe = display_message(message)
        self.assertEqual(safe["data"][0]["user"], "1234******")
        self.assertNotIn("private", json.dumps(safe))

    def test_spaces_and_sensitive_values_preserved(self):
        self.assertEqual(build_payload(spec("wifi.set"), {"ssid": " room  wifi ", "pass": " a  b "}),
                         {"op": "wifi.set", "ssid": " room  wifi ", "pass": " a  b "})

    def test_limits_and_nan(self):
        for values in ({"lat": "nan", "lng": "1", "addr": ""},
                       {"lat": "91", "lng": "1", "addr": ""}):
            with self.assertRaises(ValueError):
                build_payload(spec("loc.set"), values)
        with self.assertRaises(ValueError):
            build_payload(spec("wx.test"), {"slot": "6"})

    def test_factory_confirmation_payload(self):
        self.assertEqual(build_payload(spec("factory.reset"), {}),
                         {"op": "factory.reset", "confirm": "YES"})


class Requests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.output = []
        owner = self
        class Client:
            mtu_size = 23
            async def write_gatt_char(self, _uuid, data, response=True):
                owner.wire.extend(data)
        self.wire = bytearray()
        self.session = Session(Client(), self.output.append, lambda _m: None)

    async def reply(self, payload, message):
        task = asyncio.create_task(self.session.request(payload, timeout=1))
        await asyncio.sleep(.01)
        self.session._on_notify(None, json.dumps(message).encode())
        return await task

    async def test_handshake_negotiates_and_matches_id(self):
        message = await self.reply({"op": "hello"}, {"id": 1, "evt": "ok", "data": {"maxCommandBytes": 4095}})
        self.assertEqual(self.session.max_bytes, 4095)
        self.assertEqual(message["id"], 1)
        self.assertEqual(json.loads(self.wire)["op"], "hello")

    async def test_old_limit_prevents_write(self):
        with self.assertRaises(ValueError):
            await self.session.request({"op": "wx.import", "access": "x" * 512})
        self.assertFalse(self.wire)

    async def test_error_is_failure_and_secret_not_logged(self):
        with self.assertRaises(ValueError):
            await self.reply({"op": "wx.import", "access": "private-token"}, {"id": 1, "evt": "error"})
        self.assertNotIn("private-token", "".join(self.output))

    async def test_unrelated_event_does_not_complete_request(self):
        task = asyncio.create_task(self.session.request({"op": "status"}, timeout=.05))
        await asyncio.sleep(.01)
        self.session._on_notify(None, b'{"id":99,"evt":"ok"}')
        with self.assertRaises(TimeoutError):
            await task
        self.assertFalse(self.session.waiters)

    async def test_disconnect_aborts_pending(self):
        task = asyncio.create_task(self.session.request({"op": "status"}, timeout=1))
        await asyncio.sleep(.01)
        self.session.abort()
        with self.assertRaises(ConnectionError):
            await task


class Window(unittest.TestCase):
    def test_all_forms_render_and_close_without_bluetooth(self):
        import tkinter as tk
        root = tk.Tk()
        root.withdraw()
        app = Application(root)
        self.assertFalse(app.developer.get())
        for tab in app.developer_tabs:
            self.assertEqual(app.book.tab(tab, "state"), "hidden")
        status = {"msg": "status", "data": {"cfg": {"wifiSsid": "device network", "schedule": {"checkinAt": "22:17"}},
                  "net": {"ssid": "device network"}, "runtime": {"heapFreeBytes": 12345}}}
        app.consume_message(status)
        wifi_form = next(s for s in app.forms if any(x[1] == "wifi.set" for x in s["specs"]))
        wifi_form["choice"].current(next(i for i, x in enumerate(wifi_form["specs"]) if x[1] == "wifi.set"))
        app.render(wifi_form)
        self.assertEqual(wifi_form["vars"]["ssid"].get(), "device network")
        wifi_form["vars"]["ssid"].set("unsaved edit")
        app.consume_message(status)
        self.assertEqual(wifi_form["vars"]["ssid"].get(), "unsaved edit")
        app.consume_message({"msg": "accounts", "data": [{"label": "A01", "user": "1234******", "enabled": True}]})
        self.assertEqual(len(app.account_table.get_children()), 1)
        for state in app.forms:
            for index in range(len(state["specs"])):
                state["choice"].current(index)
                app.render(state)
                root.update_idletasks()
                self.assertEqual(set(state["vars"]), {f[0] for f in state["specs"][index][2]})
        app.developer.set(True)
        app.toggle_developer()
        for tab in app.developer_tabs:
            self.assertEqual(app.book.tab(tab, "state"), "normal")
        app.log.configure(state="normal")
        app.log.insert("end", "temporary log")
        app.developer.set(False)
        app.toggle_developer()
        self.assertEqual(app.log.get("1.0", "end").strip(), "")
        app.reset_device()
        self.assertEqual(app.snapshot, {})
        self.assertFalse(app.account_table.get_children())
        app.close()
        root.mainloop()
        app.backend.thread.join(timeout=2)
        self.assertFalse(app.backend.thread.is_alive())


if __name__ == "__main__":
    unittest.main()
