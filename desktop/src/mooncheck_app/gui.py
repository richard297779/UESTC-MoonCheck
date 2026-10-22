# -*- coding: utf-8 -*-
"""图形界面（标准库 tkinter，无额外依赖）。

布局：左侧「配置」标签页（基本 / 账号 / 推送 / 时间）+ 右侧「运行日志」。
表单改动先进入内存副本，点「保存配置」才落盘；账号与通道列表操作即时改
内存副本并标脏，退出时若有未保存改动会提示。
所有网络动作在工作线程执行，UI 线程通过队列轮询更新，避免卡窗。
"""
import copy
import datetime
import json
import os
import queue
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, ttk

from . import __version__, config, paths, push, service

_ROW_PAD = {'padx': 8, 'pady': 3}
_STYLE_TEXTS = {'0': '0 GET 模板', '1': '1 POST 文本', '2': '2 POST JSON'}


class App(object):
    def __init__(self, root):
        self.root = root
        self.cfg = config.load()
        self.work = copy.deepcopy(self.cfg)
        self.queue = queue.Queue()
        self.busy = False
        self.show_pwd = False

        root.title('MoonCheck v%s · 电子科技大学晚间打卡' % __version__)
        root.geometry('1020x620')
        root.minsize(860, 520)

        main = ttk.Frame(root)
        main.pack(fill='both', expand=True, padx=6, pady=6)
        paned = ttk.PanedWindow(main, orient='horizontal')
        paned.pack(fill='both', expand=True)

        left = ttk.Frame(paned)
        right = ttk.Frame(paned)
        paned.add(left, weight=1)
        paned.add(right, weight=1)

        self.notebook = ttk.Notebook(left)
        self.notebook.pack(fill='both', expand=True)
        self.tab_basic = ttk.Frame(self.notebook)
        self.tab_accounts = ttk.Frame(self.notebook)
        self.tab_notify = ttk.Frame(self.notebook)
        self.tab_schedule = ttk.Frame(self.notebook)
        self.notebook.add(self.tab_basic, text='基本')
        self.notebook.add(self.tab_accounts, text='账号')
        self.notebook.add(self.tab_notify, text='推送')
        self.notebook.add(self.tab_schedule, text='打卡时间')

        self._build_basic(self.tab_basic)
        self._build_accounts(self.tab_accounts)
        self._build_notify(self.tab_notify)
        self._build_schedule(self.tab_schedule)
        self._build_actions(left)
        self._build_log(right)

        self._load_form()
        self._log('配置文件：%s' % paths.config_path())
        self._refresh_status_line()
        root.protocol('WM_DELETE_WINDOW', self._on_close)
        self.root.after(80, self._drain_queue)

    # ------------------------------------------------------------ 基本页

    def _build_basic(self, parent):
        ttk.Label(parent, text='单人模式凭证（账号表为空时生效）',
                  font=('', 10, 'bold')).grid(
            row=0, column=0, columnspan=3, sticky='w', **_ROW_PAD)
        self.vars = {}
        row = 1
        self.vars['student_id'] = self._entry_row(parent, row, '学号'); row += 1
        self.pwd_var = tk.StringVar()
        self.pwd_entry = self._entry_row(parent, row, '密码', secret=True, var=self.pwd_var)
        row += 1
        self.vars['browser_fingerprint'] = self._entry_row(parent, row, '浏览器指纹'); row += 1

        sep = ttk.Separator(parent, orient='horizontal'); sep.grid(
            row=row, column=0, columnspan=3, sticky='ew', pady=6); row += 1
        ttk.Label(parent, text='默认位置（账号可各自覆盖）', font=('', 10, 'bold')).grid(
            row=row, column=0, columnspan=3, sticky='w', **_ROW_PAD); row += 1
        self.vars['location.latitude'] = self._entry_row(parent, row, '纬度'); row += 1
        self.vars['location.longitude'] = self._entry_row(parent, row, '经度'); row += 1
        self.vars['location.reverse_geocode'] = self._entry_row(parent, row, '地址文字'); row += 1

        sep = ttk.Separator(parent, orient='horizontal'); sep.grid(
            row=row, column=0, columnspan=3, sticky='ew', pady=6); row += 1
        ttk.Label(parent, text='高级', font=('', 10, 'bold')).grid(
            row=row, column=0, columnspan=3, sticky='w', **_ROW_PAD); row += 1
        self.vars['timeout'] = self._entry_row(parent, row, 'HTTP 超时(秒)'); row += 1
        self.session_var = tk.StringVar()
        ttk.Label(parent, text='会话 Cookie').grid(row=row, column=0, sticky='e')
        ttk.Entry(parent, textvariable=self.session_var, width=34).grid(
            row=row, column=1, columnspan=2, sticky='w', **_ROW_PAD)
        row += 1

    def _entry_row(self, parent, row, label, secret=False, var=None):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky='e', **_ROW_PAD)
        var = var or tk.StringVar()
        entry = ttk.Entry(parent, textvariable=var, width=34,
                          show='*' if secret else '')
        entry.grid(row=row, column=1, columnspan=2, sticky='w', **_ROW_PAD)
        if secret:
            ttk.Button(parent, text='显', width=3,
                       command=lambda: self._toggle_pwd(entry)).grid(
                row=row, column=3, sticky='w')
        return var

    def _toggle_pwd(self, entry):
        self.show_pwd = not self.show_pwd
        entry.configure(show='' if self.show_pwd else '*')

    # ------------------------------------------------------------ 账号页

    def _build_accounts(self, parent):
        ttk.Label(parent, text='多账号表（≤%d；空=单人模式）' % config.MAX_ACCOUNTS,
                  font=('', 10, 'bold')).grid(
            row=0, column=0, columnspan=2, sticky='w', **_ROW_PAD)
        self.acct_list = tk.Listbox(parent, height=7, width=42, exportselection=False)
        self.acct_list.grid(row=1, column=0, rowspan=8, padx=8, pady=4)
        self.acct_list.bind('<<ListboxSelect>>', self._on_acct_select)

        side = ttk.Frame(parent)
        side.grid(row=1, column=1, rowspan=8, sticky='n', **_ROW_PAD)
        self.acct_vars = {}
        r = 0
        for name, label in (('label', '标签'), ('student_id', '学号'),
                            ('password', '密码'), ('browser_fingerprint', '指纹')):
            self.acct_vars[name] = self._entry_row(side, r, label,
                                                   secret=(name == 'password'))
            r += 1
        self.acct_own_loc = tk.BooleanVar(value=False)
        ttk.Checkbutton(side, text='该账号使用自己的坐标',
                        variable=self.acct_own_loc).grid(
            row=r, column=0, columnspan=2, sticky='w'); r += 1
        self.acct_vars['lat'] = self._entry_row(side, r, '纬度'); r += 1
        self.acct_vars['lng'] = self._entry_row(side, r, '经度'); r += 1
        self.acct_vars['addr'] = self._entry_row(side, r, '地址'); r += 1

        btns = ttk.Frame(parent)
        btns.grid(row=9, column=0, columnspan=2, sticky='w', **_ROW_PAD)
        ttk.Button(btns, text='新增', command=self._on_acct_add).pack(side='left', padx=2)
        ttk.Button(btns, text='应用修改', command=self._on_acct_apply).pack(side='left', padx=2)
        ttk.Button(btns, text='删除', command=self._on_acct_remove).pack(side='left', padx=2)
        ttk.Button(btns, text='启用/禁用', command=self._on_acct_toggle).pack(side='left', padx=2)
        ttk.Label(parent, text='改动后记得点下方「保存配置」落盘',
                  foreground='#777').grid(row=10, column=0, columnspan=2,
                                         sticky='w', padx=8)
        self._refresh_acct_list()

    def _acct_row_text(self, acct):
        mark = '●' if acct.get('enabled', True) else '✗'
        last = ''
        if acct.get('last_msg'):
            last = '  [%s]' % str(acct.get('last_msg'))[:16]
        return '%s %-8s %s%s' % (mark, acct.get('label'),
                                 '指纹✓' if acct.get('browser_fingerprint') else '指纹✗', last)

    def _refresh_acct_list(self, keep=None):
        self.acct_list.delete(0, 'end')
        for acct in self.work.get('accounts') or []:
            self.acct_list.insert('end', self._acct_row_text(acct))
        if keep is not None and 0 <= keep < len(self.work.get('accounts') or []):
            self.acct_list.selection_set(keep)
            self.acct_list.activate(keep)

    def _acct_index(self):
        sel = self.acct_list.curselection()
        return sel[0] if sel else -1

    def _selected_acct(self):
        idx = self._acct_index()
        if idx < 0:
            return None
        accounts = self.work.get('accounts') or []
        return accounts[idx] if idx < len(accounts) else None

    def _on_acct_select(self, _event=None):
        acct = self._selected_acct()
        if not acct:
            return
        self.acct_vars['label'].set(acct.get('label') or '')
        self.acct_vars['student_id'].set(acct.get('student_id') or '')
        self.acct_vars['password'].set(acct.get('password') or '')
        self.acct_vars['browser_fingerprint'].set(acct.get('browser_fingerprint') or '')
        loc = acct.get('location')
        self.acct_own_loc.set(isinstance(loc, dict))
        loc = loc if isinstance(loc, dict) else {}
        self.acct_vars['lat'].set('' if loc.get('latitude') is None else str(loc.get('latitude')))
        self.acct_vars['lng'].set('' if loc.get('longitude') is None else str(loc.get('longitude')))
        self.acct_vars['addr'].set(loc.get('reverse_geocode') or '')

    def _acct_form_into(self, acct):
        label = self.acct_vars['label'].get().strip()
        if not label:
            raise ValueError('标签不能为空')
        import re
        if not re.match(config._LABEL, label):
            raise ValueError('标签只允许 A-Za-z0-9-_，≤12 字符')
        dup = [a for a in self.work.get('accounts') or []
               if a.get('label') == label and a is not acct]
        if dup:
            raise ValueError('标签重复：%s' % label)
        acct['label'] = label
        for key in ('student_id', 'password', 'browser_fingerprint'):
            value = self.acct_vars[key].get()
            if key == 'password':
                acct[key] = value or acct.get(key, '')
            elif value.strip() or not acct.get(key):
                acct[key] = value.strip()
        if self.acct_own_loc.get():
            loc = dict(config.LOCATION_DEFAULTS)
            try:
                if self.acct_vars['lat'].get().strip():
                    loc['latitude'] = float(self.acct_vars['lat'].get())
                if self.acct_vars['lng'].get().strip():
                    loc['longitude'] = float(self.acct_vars['lng'].get())
            except ValueError:
                raise ValueError('账号坐标必须是数字')
            if self.acct_vars['addr'].get().strip():
                loc['reverse_geocode'] = self.acct_vars['addr'].get().strip()
            acct['location'] = loc
        else:
            acct['location'] = None

    def _on_acct_add(self):
        accounts = self.work.setdefault('accounts', [])
        if len(accounts) >= config.MAX_ACCOUNTS:
            messagebox.showerror('上限', '最多 %d 个账号（与固件一致）' % config.MAX_ACCOUNTS)
            return
        accounts.append(dict(config.ACCOUNT_TEMPLATE))
        self._refresh_acct_list(keep=len(accounts) - 1)
        self.acct_vars['label'].set('')
        for k in ('student_id', 'password', 'browser_fingerprint', 'lat', 'lng', 'addr'):
            self.acct_vars[k].set('')

    def _on_acct_apply(self):
        acct = self._selected_acct()
        if acct is None:
            messagebox.showinfo('未选择', '先在左侧选中一个账号')
            return
        try:
            self._acct_form_into(acct)
        except ValueError as e:
            messagebox.showerror('账号有误', str(e))
            return
        self._refresh_acct_list(keep=self._acct_index())

    def _on_acct_remove(self):
        idx = self._acct_index()
        acct = self._selected_acct()
        if acct is None:
            return
        if not messagebox.askyesno('删除', '删除账号 %s？其会话一并清除。' % acct.get('label')):
            return
        self.work['accounts'].pop(idx)
        self._refresh_acct_list()

    def _on_acct_toggle(self):
        acct = self._selected_acct()
        if acct is None:
            return
        idx = self._acct_index()
        acct['enabled'] = not acct.get('enabled', True)
        self._refresh_acct_list(keep=idx)

    # ------------------------------------------------------------ 推送页

    def _build_notify(self, parent):
        ttk.Label(parent, text='默认通道（收所有事件：摘要/告警/汇总/单人结果）',
                  font=('', 10, 'bold')).grid(
            row=0, column=0, columnspan=3, sticky='w', **_ROW_PAD)
        self.notify_on = tk.BooleanVar(value=False)
        ttk.Checkbutton(parent, text='启用默认通道', variable=self.notify_on).grid(
            row=1, column=0, columnspan=2, sticky='w', padx=8)
        self.notify_url = tk.StringVar()
        self.notify_style = tk.StringVar(value='2 POST JSON')
        self._label_entry_row(parent, 2, '地址', self.notify_url, width=52)
        self._label_entry_row(parent, 3, '方式', self.notify_style,
                              combo=list(_STYLE_TEXTS.values()))
        ttk.Label(parent, text='Server酱 URL 形如 https://sctapi.ftqq.com/<SENDKEY>.send；'
                              '正文自动用 desp 字段',
                  foreground='#777', wraplength=430).grid(
            row=4, column=0, columnspan=3, sticky='w', padx=8)

        sep = ttk.Separator(parent, orient='horizontal'); sep.grid(
            row=5, column=0, columnspan=3, sticky='ew', pady=6)
        ttk.Label(parent, text='额外通道（≤%d，可关联具体账号，各收各的打卡结果）'
                  % config.MAX_CHANNELS, font=('', 10, 'bold')).grid(
            row=6, column=0, columnspan=3, sticky='w', **_ROW_PAD)

        self.chan_list = tk.Listbox(parent, height=5, width=40, exportselection=False)
        self.chan_list.grid(row=7, column=0, rowspan=6, padx=8, pady=4)
        self.chan_list.bind('<<ListboxSelect>>', self._on_chan_select)

        side = ttk.Frame(parent)
        side.grid(row=7, column=1, rowspan=6, sticky='n', **_ROW_PAD)
        self.chan_vars = {}
        r = 0
        self.chan_vars['label'] = self._entry_row(side, r, '标签'); r += 1
        self.chan_vars['url'] = self._entry_row(side, r, '地址'); r += 1
        self.chan_vars['style'] = self._entry_row(side, r, '方式'); r += 1
        style_combo = ttk.Combobox(side, textvariable=self.chan_vars['style'],
                                   values=list(_STYLE_TEXTS.values()), state='readonly', width=14)
        style_combo.grid(row=r - 1, column=1, columnspan=2, sticky='w', **_ROW_PAD)

        ttk.Label(side, text='关联账号').grid(row=r, column=0, sticky='ne', **_ROW_PAD)
        self.link_list = tk.Listbox(side, height=4, width=24, exportselection=False,
                                    selectmode='extended')
        self.link_list.grid(row=r, column=1, columnspan=2, sticky='w', **_ROW_PAD)
        r += 1
        self.chan_all = tk.BooleanVar(value=True)
        ttk.Checkbutton(side, text='收所有事件（含摘要/汇总，等同 all）',
                        variable=self.chan_all).grid(
            row=r, column=0, columnspan=3, sticky='w'); r += 1

        btns = ttk.Frame(parent)
        btns.grid(row=13, column=0, columnspan=3, sticky='w', **_ROW_PAD)
        ttk.Button(btns, text='新增通道', command=self._on_chan_add).pack(side='left', padx=2)
        ttk.Button(btns, text='应用修改', command=self._on_chan_apply).pack(side='left', padx=2)
        ttk.Button(btns, text='删除', command=self._on_chan_remove).pack(side='left', padx=2)
        ttk.Button(btns, text='启用/禁用', command=self._on_chan_toggle).pack(side='left', padx=2)
        ttk.Button(btns, text='发送测试', command=lambda: self._run('notify_test')).pack(
            side='left', padx=2)
        ttk.Label(parent, text='测试发到「当前选中的额外通道」；未选中时发默认通道',
                  foreground='#777').grid(row=14, column=0, columnspan=3,
                                         sticky='w', padx=8)
        self._refresh_chan_list()

    def _label_entry_row(self, parent, row, label, var, width=34, combo=None):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky='e', **_ROW_PAD)
        if combo:
            ttk.Combobox(parent, textvariable=var, values=combo,
                         state='readonly', width=16).grid(
                row=row, column=1, columnspan=2, sticky='w', **_ROW_PAD)
        else:
            ttk.Entry(parent, textvariable=var, width=width).grid(
                row=row, column=1, columnspan=2, sticky='w', **_ROW_PAD)

    def _chan_row_text(self, ch):
        mark = '●' if ch.get('on') else '✗'
        accs = ch.get('accounts') or ['all']
        return '%s %-8s %s' % (mark, ch.get('label'), ','.join(accs))

    def _refresh_chan_list(self, keep=None):
        self.chan_list.delete(0, 'end')
        for ch in (self.work.get('notify') or {}).get('channels') or []:
            self.chan_list.insert('end', self._chan_row_text(ch))
        if keep is not None and 0 <= keep < self.chan_list.size():
            self.chan_list.selection_set(keep)
            self.chan_list.activate(keep)

    def _chan_index(self):
        sel = self.chan_list.curselection()
        return sel[0] if sel else -1

    def _selected_chan(self):
        idx = self._chan_index()
        if idx < 0:
            return None
        channels = (self.work.get('notify') or {}).get('channels') or []
        return channels[idx] if idx < len(channels) else None

    def _refresh_link_list(self, selected=None):
        self.link_list.delete(0, 'end')
        for label in config.account_labels(self.work) or ['(账号表为空)']:
            self.link_list.insert('end', label)
        if selected:
            items = list(self.link_list.get(0, 'end'))
            for i, text in enumerate(items):
                if text in selected:
                    self.link_list.selection_set(i)

    def _on_chan_select(self, _event=None):
        ch = self._selected_chan()
        if not ch:
            return
        self.chan_vars['label'].set(ch.get('label') or '')
        self.chan_vars['url'].set(ch.get('url') or '')
        self.chan_vars['style'].set(_STYLE_TEXTS.get(str(ch.get('style', 2)), '2 POST JSON'))
        accounts = ch.get('accounts') or []
        self.chan_all.set((not accounts) or ('all' in accounts))
        self._refresh_link_list(selected=[a for a in accounts if a != 'all'])

    def _chan_form_into(self, ch):
        import re
        label = self.chan_vars['label'].get().strip()
        if not label:
            raise ValueError('通道标签不能为空')
        if not re.match(config._LABEL, label):
            raise ValueError('通道标签只允许 A-Za-z0-9-_，≤12 字符')
        dup = [c for c in (self.work.get('notify') or {}).get('channels') or []
               if c.get('label') == label and c is not ch]
        if dup:
            raise ValueError('通道标签重复：%s' % label)
        url = self.chan_vars['url'].get().strip()
        if url and not url.startswith('http'):
            raise ValueError('通道地址需要以 http(s):// 开头')
        ch['label'] = label
        if url:
            ch['url'] = url
        text = self.chan_vars['style'].get()
        ch['style'] = int(text.split(' ')[0]) if text else 2
        if self.chan_all.get():
            ch['accounts'] = ['all']
        else:
            picked = [self.link_list.get(i) for i in self.link_list.curselection()]
            picked = [p for p in picked if p != '(账号表为空)']
            ch['accounts'] = picked
        return ch

    def _on_chan_add(self):
        notify = self.work.setdefault('notify', dict(config.DEFAULTS['notify']))
        channels = notify.setdefault('channels', [])
        if len(channels) >= config.MAX_CHANNELS:
            messagebox.showerror('上限', '额外通道最多 %d 条' % config.MAX_CHANNELS)
            return
        channels.append(dict(config.CHANNEL_TEMPLATE))
        self._refresh_chan_list(keep=len(channels) - 1)
        for v in self.chan_vars.values():
            v.set('')
        self.chan_all.set(True)
        self._refresh_link_list()

    def _on_chan_apply(self):
        ch = self._selected_chan()
        if ch is None:
            messagebox.showinfo('未选择', '先在左侧选中一个通道')
            return
        idx = self._chan_index()
        try:
            self._chan_form_into(ch)
        except ValueError as e:
            messagebox.showerror('通道有误', str(e))
            return
        self._refresh_chan_list(keep=idx)

    def _on_chan_remove(self):
        idx = self._chan_index()
        ch = self._selected_chan()
        if ch is None:
            return
        if not messagebox.askyesno('删除', '删除通道 %s？' % ch.get('label')):
            return
        (self.work.get('notify') or {}).get('channels', []).pop(idx)
        self._refresh_chan_list()

    def _on_chan_toggle(self):
        ch = self._selected_chan()
        if ch is None:
            return
        idx = self._chan_index()
        ch['on'] = not ch.get('on', True)
        self._refresh_chan_list(keep=idx)

    def _notify_target(self):
        ch = self._selected_chan()
        if ch is not None:
            return dict(ch)
        notify = self.work.get('notify') or {}
        return {'label': '', 'on': bool(notify.get('on')) and bool(notify.get('url')),
                'style': notify.get('style', 2), 'url': notify.get('url') or '',
                'accounts': ['all']}

    # ------------------------------------------------------------ 时间页

    def _build_schedule(self, parent):
        ttk.Label(parent, text='打卡窗口（auto/打卡按钮只在窗口内执行；勾选「强制」可越窗）',
                  font=('', 10, 'bold')).grid(
            row=0, column=0, columnspan=2, sticky='w', **_ROW_PAD)
        self.sched_at = tk.StringVar()
        self.sched_end = tk.StringVar()
        self._label_entry_row(parent, 1, '开始 (HH:MM)', self.sched_at)
        self._label_entry_row(parent, 2, '截止 (HH:MM)', self.sched_end)
        self.sched_hint = ttk.Label(parent, text='', foreground='#555')
        self.sched_hint.grid(row=3, column=0, columnspan=2, sticky='w', padx=8, pady=8)
        ttk.Label(parent, text='提示：固件端 21:00 预登录是设备行为；桌面端直接按窗口执行，\n'
                              '配合系统计划任务（任务计划程序/cron）在窗口内定时跑\n'
                              '  MoonCheck auto',
                  foreground='#777', justify='left').grid(
            row=4, column=0, columnspan=2, sticky='w', padx=8)
        self._refresh_sched_hint()

    def _refresh_sched_hint(self):
        cfg = self.work
        try:
            inside = config.in_window(cfg)
            self.sched_hint.configure(
                text='现在 %s，%s窗口内' % (
                    datetime.datetime.now().strftime('%H:%M'),
                    '在' if inside else '不在'))
        except Exception:
            self.sched_hint.configure(text='时间格式应为 HH:MM')

    # ------------------------------------------------------------ 动作区

    def _build_actions(self, parent):
        opts = ttk.Frame(parent)
        opts.pack(fill='x', pady=(6, 0))
        self.dry_run_var = tk.BooleanVar(value=True)
        self.force_var = tk.BooleanVar(value=False)
        self.reverse_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(opts, text='打卡仅预览(dry-run)', variable=self.dry_run_var).pack(side='left')
        ttk.Checkbutton(opts, text='强制（越窗/重发）', variable=self.force_var).pack(side='left')
        ttk.Checkbutton(opts, text='校准地址', variable=self.reverse_var).pack(side='left')

        btns = ttk.Frame(parent)
        btns.pack(fill='x', pady=4)
        self.b_save = ttk.Button(btns, text='保存配置', command=self._on_save)
        self.b_save.pack(side='left', padx=2)
        ttk.Button(btns, text='自检', command=lambda: self._run('selftest')).pack(side='left', padx=2)
        ttk.Button(btns, text='登录', command=lambda: self._run('login')).pack(side='left', padx=2)
        ttk.Button(btns, text='查状态', command=lambda: self._run('status')).pack(side='left', padx=2)
        self.b_checkin = ttk.Button(btns, text='打卡', command=lambda: self._run('checkin'))
        self.b_checkin.pack(side='left', padx=2)
        ttk.Button(btns, text='导出配置副本', command=self._on_export).pack(side='left', padx=2)

        self.status_line = ttk.Label(parent, text='', foreground='#555')
        self.status_line.pack(fill='x', padx=4)

    def _load_form(self):
        cfg = self.work
        for key, var in self.vars.items():
            value = config.get(cfg, key)
            var.set('' if value is None else str(value))
        self.pwd_var.set(cfg.get('password') or '')
        sess = cfg.get('session_cookie') or ''
        self.session_var.set(sess)
        notify = cfg.get('notify') or {}
        self.notify_on.set(bool(notify.get('on')))
        self.notify_url.set(notify.get('url') or '')
        self.notify_style.set(_STYLE_TEXTS.get(str(notify.get('style', 2)), '2 POST JSON'))
        sched = cfg.get('schedule') or {}
        self.sched_at.set(sched.get('checkin_at') or '22:00')
        self.sched_end.set(sched.get('window_end') or '23:30')
        self._refresh_acct_list()
        self._refresh_chan_list()
        self._refresh_link_list()
        self._refresh_sched_hint()
        self._refresh_status_line()

    def _collect(self):
        """把表单值收进 self.work；返回错误列表（不抛异常）。"""
        errors = []
        cfg = self.work
        for key, var in self.vars.items():
            raw = var.get().strip()
            if raw == '':
                continue
            try:
                config.set_value(cfg, key, raw)
            except (ValueError, TypeError) as e:
                errors.append('%s: %s' % (key, e))
        if self.pwd_var.get():
            cfg['password'] = self.pwd_var.get()
        sess = self.session_var.get().strip()
        if sess.startswith('SESSION='):
            sess = sess.split('=', 1)[1]
        if sess or 'session_cookie' in cfg:
            cfg['session_cookie'] = sess
        notify = cfg.setdefault('notify', {})
        notify['on'] = self.notify_on.get()
        url = self.notify_url.get().strip()
        if url:
            notify['url'] = url
        notify['style'] = int(self.notify_style.get().split(' ')[0])
        sched = cfg.setdefault('schedule', {})
        for key, var in (('checkin_at', self.sched_at), ('window_end', self.sched_end)):
            text = var.get().strip()
            if text:
                try:
                    config.set_value(cfg, 'schedule.' + key, text)
                except (ValueError, TypeError) as e:
                    errors.append('schedule.%s: %s' % (key, e))
        return errors

    # ------------------------------------------------------------ 保存/导出

    def _on_save(self):
        errors = self._collect()
        if errors:
            messagebox.showerror('配置有误', '\n'.join(errors))
            for e in errors:
                self._log('[FAIL] ' + e)
            return
        problems = config.validate(self.work)
        errs = [p for p in problems if p[0] == 'error']
        if errs:
            messagebox.showerror('配置有误', '\n'.join('%s: %s' % (e[1], e[2]) for e in errs))
            return
        path = config.save(self.work)
        self.cfg = copy.deepcopy(self.work)
        self._log('[OK]  已保存到 %s' % path)
        for level, field, msg in config.validate(self.work):
            self._log('[%s] %s: %s' % (level.upper(), field, msg))
        self._refresh_status_line()
        self._refresh_acct_list()
        self._refresh_chan_list()
        self._refresh_sched_hint()

    def _on_export(self):
        path = filedialog.asksaveasfilename(
            title='导出配置副本', defaultextension='.json',
            initialfile='mooncheck-config.json',
            filetypes=[('JSON', '*.json')])
        if not path:
            return
        self._collect()
        cfg = copy.deepcopy(self.work)
        cfg['password'] = 'PUT_YOUR_PASSWORD'
        cfg['session_cookie'] = ''
        for acct in cfg.get('accounts') or []:
            acct['password'] = 'PUT_YOUR_PASSWORD'
            acct['session_cookie'] = ''
        with open(path, 'w', encoding='utf-8') as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
        self._log('[OK]  已导出脱敏副本（所有密码/会话已置空）：%s' % path)

    # ------------------------------------------------------------ 运行动作

    def _run(self, action):
        if self.busy:
            self._log('[..] 有任务正在执行，请稍候')
            return
        errors = self._collect()
        if errors:
            messagebox.showerror('配置有误', '\n'.join(errors))
            return
        cfg = copy.deepcopy(self.work)
        self.busy = True
        self._set_buttons_enabled(False)
        self._log('—— %s 开始 ——' % action)

        def job():
            log = self.queue.put
            try:
                if action == 'selftest':
                    res = service.selftest(cfg, log)
                elif action == 'login':
                    res = service.do_login(cfg, log)
                elif action == 'status':
                    res = service.do_status(cfg, log)
                elif action == 'checkin':
                    res = service.do_checkin(cfg, log, force=self.force_var.get(),
                                             dry_run=self.dry_run_var.get(),
                                             use_reverse=self.reverse_var.get())
                elif action == 'notify_test':
                    target = self._notify_target()
                    target = copy.deepcopy(target)
                    ok, note = push.send(target, '推送测试',
                                         'MoonCheck 桌面端测试消息 %s' %
                                         datetime.datetime.now().strftime('%H:%M:%S'),
                                         timeout=cfg.get('timeout') or 15)
                    res = service.Result('ok' if ok else 'fail', note)
                else:
                    res = service.Result('fail', '未知动作 ' + action)
            except Exception as e:  # 兜底：工作线程不崩窗
                res = service.Result('fail', '异常：%s' % e)
            self.queue.put(('__result__', res))

        threading.Thread(target=job, daemon=True).start()

    # ------------------------------------------------------------ 日志/线程汇合

    def _drain_queue(self):
        try:
            while True:
                item = self.queue.get_nowait()
                if isinstance(item, tuple) and item and item[0] == '__result__':
                    res = item[1]
                    self._log('%s %s' % ({'ok': '[OK]', 'warn': '[WARN]', 'fail': '[FAIL]',
                                          'mfa': '[MFA]', 'not_configured': '[NEED]'}
                                         .get(res.kind, '[..]'), res.text))
                    for name, value in res.rows:
                        self._log('  - %s: %s' % (name, value))
                    if res.kind == 'mfa':
                        messagebox.showwarning(
                            '需要二次验证',
                            '服务端要求二次验证：浏览器指纹未被信任。\n'
                            '请按 desktop/README.md「获取指纹」一节，为对应账号填入其浏览器指纹后重试。')
                    if res.kind in ('ok', 'warn'):
                        reloaded = config.load(create=False)
                        if reloaded != self.cfg:
                            self.cfg = reloaded
                            self.work = copy.deepcopy(reloaded)
                            self._load_form()
                            self._log('[..] 会话/结果已自动写回 config.json')
                    self.busy = False
                    self._set_buttons_enabled(True)
                    self._refresh_status_line()
                else:
                    self._log(str(item))
        except queue.Empty:
            pass
        self.root.after(80, self._drain_queue)

    def _log(self, msg):
        self.text.configure(state='normal')
        self.text.insert('end', '%s %s\n' % (
            datetime.datetime.now().strftime('%H:%M:%S'), msg))
        self.text.see('end')
        self.text.configure(state='disabled')

    def _build_log(self, parent):
        ttk.Label(parent, text='运行日志', font=('', 10, 'bold')).pack(anchor='w', **_ROW_PAD)
        self.text = scrolledtext.ScrolledText(parent, state='disabled', wrap='word',
                                              font=('Consolas', 10))
        self.text.pack(fill='both', expand=True, padx=6, pady=2)
        bar = ttk.Frame(parent)
        bar.pack(fill='x', padx=6, pady=2)
        ttk.Button(bar, text='清空日志', command=self._clear_log).pack(side='left')
        ttk.Button(bar, text='打开配置所在目录', command=self._open_dir).pack(side='left', padx=6)

    def _clear_log(self):
        self.text.configure(state='normal')
        self.text.delete('1.0', 'end')
        self.text.configure(state='disabled')

    def _open_dir(self):
        d = paths.app_dir()
        try:
            if sys.platform.startswith('win'):
                os.startfile(d)  # type: ignore[attr-defined]
            elif sys.platform == 'darwin':
                os.system('open "%s"' % d)
            else:
                os.system('xdg-open "%s" >/dev/null 2>&1 &' % d)
        except Exception as e:
            self._log('[FAIL] 打开目录失败：%s' % e)

    def _set_buttons_enabled(self, enabled):
        for b in (self.b_save, self.b_checkin):
            b.configure(state='normal' if enabled else 'disabled')

    def _refresh_status_line(self):
        cfg = self.work
        parts = []
        accounts = cfg.get('accounts') or []
        if accounts:
            enabled = [a for a in accounts if a.get('enabled', True)]
            parts.append('账号:%d个(启用%d)' % (len(accounts), len(enabled)))
        else:
            parts.append('学号:' + (cfg.get('student_id') or '未填'))
            parts.append('指纹:' + ('已填' if cfg.get('browser_fingerprint') else '未填'))
        sess = sum(1 for a in accounts if a.get('session_cookie'))
        if cfg.get('session_cookie'):
            sess += 1
        parts.append('会话:%d/%d' % (sess, max(1, len(accounts) or 1)))
        sched = cfg.get('schedule') or {}
        parts.append('窗口:%s-%s' % (sched.get('checkin_at'), sched.get('window_end')))
        channels = [c for c in config.iter_channels(cfg)
                    if c.get('on') and (c.get('url') or '').strip()]
        parts.append('推送:%s' % ('开(%d通道)' % len(channels) if channels else '关'))
        self.status_line.configure(text='  |  '.join(parts))

    def _on_close(self):
        dirty = _dict_diff(self.work, self.cfg)
        try:
            dirty = dirty or bool(self._collect_errors_only())
        except Exception:
            pass
        if dirty:
            if not messagebox.askyesno('未保存', '配置有未保存的改动，仍要退出吗？'):
                return
        self.root.destroy()

    def _collect_errors_only(self):
        snapshot = copy.deepcopy(self.work)
        errors = self._collect()
        self.work = snapshot
        return errors


def _dict_diff(a, b):
    return json.dumps(a, sort_keys=True, ensure_ascii=False, default=str) != \
        json.dumps(b, sort_keys=True, ensure_ascii=False, default=str)


def run_gui():
    root = tk.Tk()
    App(root)
    root.mainloop()
    return 0
