"""Tk component smoke tests, withdrawn window and isolated temporary config."""
import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from mooncheck_app import config, gui, service


class GuiTests(unittest.TestCase):
    def setUp(self):
        self.folder=tempfile.TemporaryDirectory()
        self.path=Path(self.folder.name)/'config.json'
        self.env=patch.dict(os.environ,MOONCHECK_CONFIG=str(self.path));self.env.start()
        cfg=config.default_config();cfg.update(student_id='fixture-student',password='fixture-password')
        config.save(cfg)
        try:
            self.root=gui.tk.Tk();self.root.withdraw()
        except gui.tk.TclError:
            self.env.stop();self.folder.cleanup();self.skipTest('Tk display unavailable')
        self.app=gui.App(self.root)

    def tearDown(self):
        self.root.destroy();self.env.stop();self.folder.cleanup()

    def test_authorization_page_and_hidden_inputs(self):
        self.assertIn('微信授权',[self.app.notebook.tab(t,'text') for t in self.app.notebook.tabs()])
        self.assertEqual(('self',),tuple(self.app.wx_picker['values']))
        entries=[w for w in self.app.tab_wechat.winfo_children() if isinstance(w,gui.ttk.Entry) and not isinstance(w,gui.ttk.Combobox)]
        self.assertEqual(['*','*','*',''],[entry.cget('show') for entry in entries])

    def test_import_persists_only_selected_account_and_clears_inputs(self):
        for key,value in dict(openid='fixture-openid',access='fixture-access',refresh='fixture-refresh',expires_at='0').items():self.app.wx_vars[key].set(value)
        self.app._wx_import()
        self.assertEqual('fixture-openid',config.load()['wechat']['openid'])
        self.assertEqual('',self.app.wx_vars['access'].get())
        target=Path(self.folder.name)/'redacted.json'
        with patch.object(gui.filedialog,'asksaveasfilename',return_value=str(target)):self.app._on_export()
        self.assertNotIn('fixture-access',target.read_text(encoding='utf-8'))
        self.assertNotIn('fixture-password',target.read_text(encoding='utf-8'))

    def test_failed_operation_reloads_rotated_credentials(self):
        def action(cfg,log,label=None):
            account=config.effective_accounts(cfg)[0]
            config.save_account_wechat(cfg,account,dict(openid='fixture',access='rotated',refresh='fixture',expires_at=0))
            return service.Result('fail','simulated failure after rotation')
        with patch.object(service,'do_wechat_test',side_effect=action):
            self.app._run('wx_test')
            end=time.monotonic()+3
            while self.app.busy and time.monotonic()<end:
                self.root.update();time.sleep(.01)
        self.assertFalse(self.app.busy)
        self.assertEqual('rotated',self.app.work['wechat']['access'])


if __name__=='__main__':unittest.main()
