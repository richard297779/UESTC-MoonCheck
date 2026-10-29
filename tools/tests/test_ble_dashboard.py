import sys
import tkinter as tk
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from ble_dashboard import Dashboard
from ble_gui import display_message


class DashboardLayout(unittest.TestCase):
    def setUp(self):
        self.root=tk.Tk()
        self.root.withdraw()
        with patch('ble_dashboard.Backend'):
            self.app=Dashboard(self.root)
        self.app.connected=True
        self.snapshot=dict(revision=10,accounts=[dict(slot=0,label='A',user='fixture-user',enabled=True,passwordSet=True,fingerprintSet=True)])
        self.app.consume(dict(msg='account snapshot',data=self.snapshot))

    def tearDown(self):
        self.app.closing=True
        for timer in self.root.tk.call('after','info'):self.root.after_cancel(timer)
        self.root.destroy()

    def select(self,category,op):
        app=self.app;app.category.set(category);app.category_changed()
        app.operation.current(next(i for i,s in enumerate(app.groups[category]) if s[1]==op));app.render_operation()

    def test_tabs_and_every_operation_render(self):
        self.assertEqual([self.app.book.tab(t,'text') for t in self.app.book.tabs() if self.app.book.tab(t,'state')!='hidden'],['设备概览','账号列表','查询与配置'])
        self.app.developer.set(True);self.app.toggle_developer()
        for category,specs in self.app.groups.items():
            for spec in specs:
                self.select(category,spec[1]);self.root.update_idletasks()

    def test_target_snapshot_change_invalidates_qr_and_selection(self):
        self.select('微信授权','wx.import');app=self.app
        app.match_text.set('A');app.resolve_target();app.check_target()
        app.state['vars']['access'].set('fixture-token')
        app.consume(dict(msg='account snapshot',data=dict(self.snapshot,revision=11)))
        self.assertEqual(app.state['vars']['access'].get(),'')
        with self.assertRaises(ValueError):app.check_target()

    def test_full_id_in_table_but_credentials_redacted(self):
        safe=display_message(dict(msg='account snapshot',data=dict(self.snapshot,accounts=[dict(self.snapshot['accounts'][0],**{'pass':'fixture-secret'})])))
        self.assertEqual(safe['data']['accounts'][0]['user'],'fixture-user')
        self.assertNotIn('fixture-secret',str(safe))
        self.assertEqual(self.app.account_table.item('0','values')[2],'fixture-user')

    def test_wifi_keeps_duplicate_and_hidden_networks(self):
        self.app.display_result(dict(data=dict(aps=[dict(ssid='same',bssid='a'),dict(ssid='same',bssid='b'),dict(ssid='',bssid='c')])))
        self.assertEqual(len(self.app.result_table.get_children()),3)

    def test_device_defaults_update_only_clean_form(self):
        self.select('网络','wifi.set');app=self.app
        app.consume(dict(msg='status',data=dict(cfg=dict(wifiSsid='device wifi'))))
        self.assertEqual(app.state['vars']['ssid'].get(),'device wifi')
        app.state['vars']['ssid'].set('user edit')
        app.consume(dict(msg='status',data=dict(cfg=dict(wifiSsid='new device wifi'))))
        self.assertEqual(app.state['vars']['ssid'].get(),'user edit')


if __name__=='__main__':unittest.main()
