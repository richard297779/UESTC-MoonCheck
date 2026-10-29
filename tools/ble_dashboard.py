"""Four-column BLE dashboard. Protocol work stays on the shared worker loop."""
import datetime
import json
import queue
import tkinter as tk
from tkinter import messagebox, ttk
from tkinter.scrolledtext import ScrolledText

from ble_gui import Application, Backend, OPERATIONS, build_payload, device_defaults, project_version
from console_model import notification_labels, patch_payload, resolve_account, selected_payload, snapshot_accounts


def menu_groups():
    groups = {
        '状态查询': [OPERATIONS['状态'][1],OPERATIONS['状态'][0]]+OPERATIONS['状态'][2:-1],
        '网络': OPERATIONS['网络'],
        '账号配置': [s for s in OPERATIONS['账号与位置'] if s[1] in ('account.add', 'account.enable', 'account.remove', 'account.clear')],
        '微信授权': OPERATIONS['微信授权'],
        '调度与执行': OPERATIONS['调度与执行'],
        '通知配置': OPERATIONS['通知'],
        '设备设置': OPERATIONS['账号与位置'][:2] + OPERATIONS['开发者'][-1:],
        '开发者工具': OPERATIONS['状态'][-1:] + OPERATIONS['开发者'][:-1] + [('原始 JSON', 'raw', [], '原始指令可能修改设备或提交打卡。')],
    }
    groups['账号配置']=[(s[0],s[1],s[2],'仅修改有新输入的项；修改学号需重新导入微信授权。' if s[1]=='account.add' else s[3]) for s in groups['账号配置']]
    return groups


class Dashboard(Application):
    def __init__(self, root):
        self.root = root
        self.events = queue.Queue()
        self.backend = Backend(self.events)
        self.busy = self.connected = self.closing = False
        self.snapshot, self.account_snapshot = {}, {}
        self.accounts, self.devices, self.forms = [], [], []
        self.qr_dialog = None
        self.current_op = ''
        self.pending_resolve = False
        self.target = None
        self.developer = tk.BooleanVar(value=False)
        self.account_mode = tk.StringVar(value='update')
        self.match_kind = tk.StringVar(value='label')
        self.match_text = tk.StringVar()
        self.match_timer = None
        self.selected = {}
        self.result_progress = {}
        root.title(f'MoonCheck {project_version()} · 蓝牙中控台')
        root.geometry(f'{min(1440, root.winfo_screenwidth()-60)}x{min(1000,root.winfo_screenheight()-80)}')
        root.minsize(1080, 740)
        style = ttk.Style(root)
        if 'clam' in style.theme_names(): style.theme_use('clam')
        style.configure('TFrame', background='#f3f6fb')
        style.configure('TLabel', background='#f3f6fb', foreground='#243c59', font=('Microsoft YaHei UI', 10))
        style.configure('TButton', padding=(12, 7))
        style.configure('TNotebook.Tab', padding=(18, 9))
        style.configure('Treeview', rowheight=34, font=('Microsoft YaHei UI', 10))
        style.configure('Treeview.Heading', font=('Microsoft YaHei UI', 10, 'bold'), padding=(7, 8))
        outer = ttk.Frame(root, padding=20)
        outer.pack(fill='both', expand=True)
        ttk.Label(outer, text='MoonCheck 蓝牙中控台', font=('Microsoft YaHei UI', 20, 'bold')).pack(anchor='w')
        ttk.Label(outer, text='设备与账号，统一管理', foreground='#72829a').pack(anchor='w', pady=(3,12))
        bar = ttk.Frame(outer)
        bar.pack(fill='x')
        self.device = ttk.Combobox(bar, state='readonly', width=47)
        self.device.pack(side='left', padx=(0,8))
        ttk.Button(bar, text='扫描设备', command=lambda:self.submit(self.backend.scan(),'scan')).pack(side='left')
        ttk.Button(bar, text='连接', command=self.connect).pack(side='left', padx=6)
        ttk.Button(bar, text='断开', command=lambda:self.submit(self.backend.disconnect(),'disconnect')).pack(side='left')
        ttk.Button(bar, text='帮助', command=self.help).pack(side='right')
        ttk.Checkbutton(bar, text='开发者选项', variable=self.developer, command=self.toggle_developer).pack(side='right', padx=12)
        self.status = tk.StringVar(value='未连接 · 请扫描并选择本人设备')
        ttk.Label(outer, textvariable=self.status, foreground='#2464b2').pack(anchor='w', pady=12)
        self.book = ttk.Notebook(outer)
        self.book.pack(fill='both', expand=True)
        self.overview_tab, self.account_tab, self.config_tab, self.receipt_tab = [ttk.Frame(self.book,padding=12) for _ in range(4)]
        for tab,title in zip((self.overview_tab,self.account_tab,self.config_tab,self.receipt_tab),('设备概览','账号列表','查询与配置','完整回执')):
            self.book.add(tab,text=title)
        self.book.hide(self.receipt_tab)
        row=ttk.Frame(self.overview_tab);row.pack(fill='x',pady=(0,10))
        ttk.Label(row,text='配置与运行状态',font=('Microsoft YaHei UI',14,'bold')).pack(side='left')
        ttk.Button(row,text='刷新状态',command=lambda:self.execute_payload({'op':'status'})).pack(side='right')
        self.overview_table = self.make_table(self.overview_tab, ('项目','当前值','说明'), (210,400,420))
        self.updated = tk.StringVar(value='尚未读取设备状态')
        ttk.Label(self.overview_tab,textvariable=self.updated).pack(anchor='w',pady=8)
        row=ttk.Frame(self.account_tab);row.pack(fill='x',pady=(0,10))
        ttk.Label(row,text='序号从 0 开始，与微信槽位一致；学号完整显示。').pack(side='left')
        ttk.Button(row,text='刷新列表',command=self.refresh).pack(side='right')
        ttk.Button(row,text='添加 / 修改账号',command=self.open_account).pack(side='right',padx=8)
        self.account_table = self.make_table(self.account_tab,('序号','账号标签','学号','密码配置','浏览器指纹','微信授权','通知渠道配置','打卡启用','最近打卡结果'),(50,90,150,90,110,100,190,100,230))
        self.account_table.bind('<ButtonRelease-1>',self.toggle_account_click)
        self.account_table.bind('<space>',self.toggle_account_key)
        self.account_hint = tk.StringVar(value='连接后读取账号；点击启用列或按空格修改勾选，回执确认后生效。')
        ttk.Label(self.account_tab,textvariable=self.account_hint).pack(anchor='w',pady=8)
        lower=ttk.LabelFrame(self.config_tab,text='查询 / 配置结果',padding=8,height=245)
        lower.pack(side='bottom',fill='x',pady=(12,0));lower.pack_propagate(False)
        self.result_meta=tk.StringVar(value='选择操作后，结果统一显示在这里')
        ttk.Label(lower,textvariable=self.result_meta).pack(anchor='w',pady=(0,5))
        self.result_table=self.make_table(lower,('项目','结果'),(230,800))
        upper=ttk.Frame(self.config_tab);upper.pack(fill='both',expand=True)
        selectors=ttk.Frame(upper);selectors.pack(fill='x',pady=(0,10))
        self.category=ttk.Combobox(selectors,state='readonly',width=20)
        self.category.pack(side='left',padx=(0,12))
        self.operation=ttk.Combobox(selectors,state='readonly',width=30)
        self.operation.pack(side='left')
        self.category.bind('<<ComboboxSelected>>',lambda _e:self.category_changed())
        self.operation.bind('<<ComboboxSelected>>',lambda _e:self.render_operation())
        self.actions=ttk.Frame(upper);self.actions.pack(side='bottom',fill='x',pady=(8,0))
        self.execute_button=ttk.Button(self.actions,text='查询 / 保存',command=self.send_current)
        self.execute_button.pack(side='right')
        self.qr_button=ttk.Button(self.actions,text='微信扫码获取授权…',command=lambda:self.start_qr(self.state))
        self.form_note=tk.StringVar()
        ttk.Label(self.actions,textvariable=self.form_note,wraplength=760,foreground='#70819a').pack(side='left')
        canvas_frame=ttk.Frame(upper);canvas_frame.pack(fill='both',expand=True)
        self.canvas=tk.Canvas(canvas_frame,background='#f3f6fb',highlightthickness=0)
        scroll=ttk.Scrollbar(canvas_frame,orient='vertical',command=self.canvas.yview)
        scroll.pack(side='right',fill='y');self.canvas.pack(side='left',fill='both',expand=True)
        self.canvas.configure(yscrollcommand=scroll.set)
        self.form_frame=ttk.Frame(self.canvas)
        canvas_id=self.canvas.create_window((0,0),window=self.form_frame,anchor='nw')
        self.form_frame.bind('<Configure>',lambda _e:self.canvas.configure(scrollregion=self.canvas.bbox('all')))
        self.canvas.bind('<Configure>',lambda e:self.canvas.itemconfigure(canvas_id,width=e.width))
        self.log=ScrolledText(self.receipt_tab,font=('Consolas',10),state='disabled')
        self.log.pack(fill='both',expand=True)
        ttk.Button(self.receipt_tab,text='清空回执',command=self.clear_log).pack(anchor='e',pady=8)
        self.raw = None
        self.populate_categories()
        root.protocol('WM_DELETE_WINDOW',self.close)
        root.after(80,self.poll)

    @staticmethod
    def make_table(parent, columns, widths):
        frame=ttk.Frame(parent);frame.pack(fill='both',expand=True)
        tree=ttk.Treeview(frame,columns=columns,show='headings')
        for column,width in zip(columns,widths):
            tree.heading(column,text=column);tree.column(column,width=width,minwidth=width//2,stretch=True)
        y=ttk.Scrollbar(frame,orient='vertical',command=tree.yview)
        x=ttk.Scrollbar(frame,orient='horizontal',command=tree.xview)
        tree.configure(yscrollcommand=y.set,xscrollcommand=x.set)
        tree.grid(row=0,column=0,sticky='nsew');y.grid(row=0,column=1,sticky='ns');x.grid(row=1,column=0,sticky='ew')
        frame.rowconfigure(0,weight=1);frame.columnconfigure(0,weight=1)
        return tree

    def populate_categories(self):
        old=self.category.get()
        self.groups=menu_groups()
        if not self.developer.get():self.groups.pop('开发者工具')
        self.category['values']=list(self.groups)
        self.category.set(old if old in self.groups else '状态查询')
        self.category_changed()

    def toggle_developer(self):
        if self.developer.get():self.book.add(self.receipt_tab)
        else:self.book.hide(self.receipt_tab);self.clear_log()
        self.populate_categories()

    def category_changed(self):
        specs=self.groups[self.category.get()]
        self.operation['values']=[s[0] for s in specs]
        self.operation.current(0)
        self.render_operation()

    def current_spec(self):
        return self.groups[self.category.get()][self.operation.current()]

    def open_account(self):
        self.account_mode.set('update')
        self.book.select(self.config_tab);self.category.set('账号配置');self.category_changed()
        selection=self.account_table.selection()
        if selection:
            row=next((a for a in self.accounts if str(a['slot'])==selection[0]),None)
            if row:self.match_kind.set('label');self.match_text.set(row['label']);self.resolve_target()

    def render_operation(self):
        self.cancel_qr()
        if self.match_timer:self.root.after_cancel(self.match_timer);self.match_timer=None
        for widget in self.form_frame.winfo_children():widget.destroy()
        self.target=None;self.target_revision=None;self.selected={};self.raw=None;self.target_group=None
        spec=self.current_spec();op=spec[1]
        self.state={'specs':self.groups[self.category.get()],'choice':self.operation,'vars':{},'dirty':False}
        self.forms=[self.state]
        self.form_note.set(spec[3] if op!='account.add' else '已有账号：仅提交非空字段。新建账号：标签、学号、密码、指纹必填。')
        self.qr_button.pack_forget()
        ttk.Label(self.form_frame,text=spec[0],font=('Microsoft YaHei UI',13,'bold')).pack(anchor='w',pady=(0,10))
        if op=='raw':
            self.raw=ScrolledText(self.form_frame,height=6,font=('Consolas',11));self.raw.pack(fill='x');self.raw.insert('1.0','{"op":"status"}');return
        if op=='account.add':
            row=ttk.Frame(self.form_frame);row.pack(fill='x',pady=(0,8))
            for value,title in [('update','修改已有账号'),('create','新建账号')]:
                ttk.Radiobutton(row,text=title,variable=self.account_mode,value=value,command=self.render_operation).pack(side='left',padx=(0,18))
        needs_target=op in ('wx.import','wx.test','account.enable','account.remove') or (op=='account.add' and self.account_mode.get()=='update')
        if needs_target:
            row=ttk.Frame(self.form_frame);row.pack(fill='x',pady=(0,7))
            ttk.Label(row,text='按').pack(side='left')
            self.kind_choice=ttk.Combobox(row,state='readonly',values=['标签','序号','学号'],width=7)
            self.kind_choice.current(0);self.kind_choice.pack(side='left',padx=7)
            self.target_entry=ttk.Entry(row,textvariable=self.match_text);self.target_entry.pack(side='left',fill='x',expand=True)
            ttk.Button(row,text='刷新并匹配',command=self.refresh_target).pack(side='left',padx=8)
            if op.startswith('wx.'):
                self.target_entry.bind('<KeyRelease>',self.auto_match)
                self.kind_choice.bind('<<ComboboxSelected>>',self.auto_match)
            self.resolved=tk.StringVar(value='请匹配唯一目标；保存前由设备验证账号快照')
            ttk.Label(self.form_frame,textvariable=self.resolved,foreground='#15816c').pack(anchor='w',pady=(0,8))
        targeted=op in ('test.login','test.chain','test.checkin','test.multi')
        if targeted:
            group=ttk.LabelFrame(self.form_frame,text='目标账号 · 可多选（仅已启用账号）',padding=8);group.pack(fill='x',pady=(0,8))
            self.target_group=group
            self.rebuild_targets()
        fields=ttk.Frame(self.form_frame);fields.pack(fill='x')
        for c in range(4):fields.columnconfigure(c,weight=1)
        self.hints={}
        index=0
        for key,title,kind,initial in spec[2]:
            if needs_target and key in ('slot','label') and op!='account.add':
                self.state['vars'][key]=tk.StringVar(value='0' if key=='slot' else '');continue
            initial=device_defaults(op,self.snapshot).get(key,initial)
            if op=='account.add':initial=''
            var=tk.BooleanVar(value=initial) if kind=='bool' else tk.StringVar(value=initial)
            self.state['vars'][key]=var
            cell=ttk.Frame(fields,padding=(0,0,12,8));cell.grid(row=index//4,column=index%4,sticky='nsew');index+=1
            if op=='account.add' and key=='bfp':title='浏览器指纹（新建必填）'
            ttk.Label(cell,text=title,wraplength=250).pack(anchor='w')
            widget=ttk.Checkbutton(cell,variable=var) if kind=='bool' else ttk.Entry(cell,textvariable=var,show='•' if kind=='secret' else '')
            var.trace_add('write',lambda *_args:self.state.update(dirty=True))
            widget.pack(fill='x',pady=4)
            hint=tk.StringVar(value='留空保留原值' if op=='account.add' and self.account_mode.get()=='update' else '')
            self.hints[key]=hint
            ttk.Label(cell,textvariable=hint,foreground='#8190a3',wraplength=250,font=('Microsoft YaHei UI',8)).pack(anchor='w')
        if op=='wx.import':self.qr_button.pack(side='right',padx=8)
        self.state['dirty']=False
        self.canvas.yview_moveto(0)

    def rebuild_targets(self):
        if not self.target_group:return
        for child in self.target_group.winfo_children():child.destroy()
        self.selected={}
        for i,account in enumerate(self.accounts):
            var=tk.BooleanVar(value=False);self.selected[account['label']]=var
            ttk.Checkbutton(self.target_group,text=f"#{account['slot']} · {account['label']} · {account.get('user','')}",variable=var,state='normal' if account.get('enabled') else 'disabled').grid(row=i//2,column=i%2,sticky='w',padx=12,pady=5)
        if not self.accounts:ttk.Label(self.target_group,text='请先连接并读取账号表').grid(row=0,column=0)
        else:ttk.Button(self.target_group,text='全选已启用账号',command=lambda:[self.selected[a['label']].set(bool(a.get('enabled'))) for a in self.accounts]).grid(row=(len(self.accounts)+1)//2,column=0,sticky='w')

    def check_target(self):
        if not self.target or self.target_revision!=self.account_snapshot.get('revision'):
            raise ValueError('账号表已变化或尚未匹配，请刷新并重新匹配目标')
        kind={'标签':'label','序号':'slot','学号':'user'}.get(self.kind_choice.get(),'auto')
        row=resolve_account(self.accounts,self.match_text.get(),kind)
        if any(row.get(k)!=self.target.get(k) for k in ('label','slot','user')):
            raise ValueError('目标已变化，请重新匹配账号')

    def start_qr(self,state):
        try:self.require_v2();self.check_target()
        except ValueError as error:messagebox.showinfo('请先匹配账号',str(error));return
        super().start_qr(state)

    def auto_match(self,_event=None):
        if self.match_timer:self.root.after_cancel(self.match_timer)
        self.cancel_qr()
        for key in ('openid','access','refresh'):
            if key in self.state['vars']:self.state['vars'][key].set('')
        self.target=None
        self.match_timer=self.root.after(500,self.refresh_target)

    def refresh_target(self):
        self.match_timer=None
        if not self.connected or self.busy:return
        self.pending_resolve=True
        self.submit(self.backend.refresh(),'resolve')

    def resolve_target(self):
        kind={'标签':'label','序号':'slot','学号':'user'}.get(self.kind_choice.get(),'auto')
        try:
            self.target_revision=self.account_snapshot.get('revision')
            self.target=resolve_account(self.accounts,self.match_text.get(),kind)
            self.resolved.set(f"已匹配 #{self.target['slot']} · {self.target['label']} · {self.target.get('user','')}，微信槽位 {self.target['slot']}")
            if 'slot' in self.state['vars']:self.state['vars']['slot'].set(str(self.target['slot']))
            if 'label' in self.state['vars'] and self.current_spec()[1]!='account.add':self.state['vars']['label'].set(self.target['label'])
            if self.current_spec()[1]=='account.add':
                for key,hint in self.hints.items():
                    value=self.target.get({'pass':'passwordSet','bfp':'fingerprintSet'}.get(key,key),'设备未提供')
                    if key in ('pass','bfp'):value='已配置' if value else '未配置'
                    hint.set('当前：'+str(value)+' · 留空不改')
        except ValueError as error:
            self.target=None;self.resolved.set(str(error))

    def require_v2(self):
        if 'revision' not in self.account_snapshot:raise ValueError('当前固件不支持安全账号管理，请升级带 account-v2 能力的固件')

    def send_current(self):
        if not self.connected or self.busy:
            messagebox.showinfo('尚不可操作','请先连接，并等待当前操作完成。');return
        spec=self.current_spec();op=spec[1]
        try:
            values={k:v.get() for k,v in self.state['vars'].items()}
            if op=='account.add':
                self.require_v2()
                if self.account_mode.get()=='update':self.check_target()
                payload=patch_payload(self.account_snapshot,values,self.target,self.account_mode.get()=='create')
            elif op in ('test.login','test.chain','test.checkin','test.multi'):
                self.require_v2();base=build_payload(spec,values);base.pop('op')
                targets=[a for a in self.accounts if self.selected.get(a['label']) and self.selected[a['label']].get()]
                payload=selected_payload(self.account_snapshot,targets,op,**base)
            elif op=='raw':
                payload=json.loads(self.raw.get('1.0','end'))
                if not isinstance(payload,dict) or not isinstance(payload.get('op'),str):raise ValueError('请输入含 op 的 JSON 对象')
            else:
                payload=build_payload(spec,values)
                if op in ('wx.import','wx.test','account.enable','account.remove'):
                    self.require_v2()
                    self.check_target()
                    payload['expectedRevision']=self.account_snapshot['revision']
                    if op.startswith('wx.'):payload.update(target=self.target['label'],slot=self.target['slot'])
                    else:
                        if self.target.get('primary'):raise ValueError('主账号请在设备设置中管理')
                        payload['label']=self.target['label']
                if op=='account.clear':self.require_v2();payload['expectedRevision']=self.account_snapshot['revision']
            if spec[3] or op in ('account.add','account.clear'):
                target_text='、'.join(payload.get('targets',[])) or payload.get('target',payload.get('label','当前设备'))
                message=f"设备：{self.device.get()}\n目标：{target_text}\n操作：{spec[0]}\n{spec[3]}"
                if op in ('account.remove','account.clear'):message+='\n删除或清空账号后将清除微信授权，需重新导入，避免槽位错配。'
                if not messagebox.askyesno('确认执行',message):return
            self.execute_payload(payload)
            for key,_title,kind,_initial in spec[2]:
                if kind=='secret' and key in self.state['vars']:self.state['vars'][key].set('')
            if self.raw:self.raw.delete('1.0','end')
        except (ValueError,TypeError,KeyError) as error:
            # Parser errors can echo entered values. Only our fixed messages are shown.
            text=str(error)
            if 'could not convert' in text or isinstance(error,KeyError):text='请检查数字格式与必填字段'
            messagebox.showerror('未发送',text)

    def execute_payload(self,payload):
        if self.busy or not self.connected:return
        self.current_op=payload['op'];self.result_progress={}
        self.changed_fields=list(payload.get('fields',{}))
        self.show_rows(('项目','结果'),[('操作',self.current_op),('状态','等待设备回执…')])
        self.submit(self.backend.request(payload),'request')

    def refresh(self):
        if self.connected and not self.busy:self.submit(self.backend.refresh(),'refresh')

    def toggle_account_key(self,_event=None):
        selection=self.account_table.selection()
        if selection:self.toggle_account(selection[0])
        return 'break'

    def toggle_account_click(self,event):
        if self.account_table.identify_column(event.x)=='#8':
            row=self.account_table.identify_row(event.y)
            if row:self.toggle_account(row)

    def toggle_account(self,slot):
        if self.busy or not self.connected:return
        try:
            self.require_v2()
            row=next(a for a in self.accounts if str(a['slot'])==slot)
            if row.get('primary'):raise ValueError('单人模式请使用调度中的自动打卡开关')
            self.account_table.set(slot,'打卡启用','等待确认…')
            self.execute_payload(dict(op='account.enable',label=row['label'],on=not row['enabled'],expectedRevision=self.account_snapshot['revision']))
        except (ValueError,StopIteration) as error:messagebox.showinfo('未修改',str(error))

    def show_rows(self,columns,rows):
        tree=self.result_table;tree.delete(*tree.get_children());tree['columns']=columns
        for column in columns:tree.heading(column,text=column);tree.column(column,width=max(100,1000//len(columns)),minwidth=75)
        for row in rows:tree.insert('','end',values=[str(x) if x is not None else '设备未提供' for x in row])
        self.result_meta.set(datetime.datetime.now().strftime('%H:%M:%S')+' · '+(self.current_op or '设备查询'))

    def flatten(self,data,prefix=''):
        labels={'ssid':'网络名','ip':'IP 地址','rssi':'信号强度','enabled':'启用','label':'账号标签','user':'学号','passwordSet':'密码已配置','fingerprintSet':'指纹已配置','revision':'账号修订号','slot':'微信槽位','expiresAt':'到期 Unix 秒','msg':'说明','ok':'成功','lat':'纬度','lng':'经度','addr':'地址','notificationLinksUpdated':'通知关联已更新','authorizationCleared':'微信授权已清除','fw':'固件版本','proto':'协议版本','built':'构建时间','boardId':'板型','chipModel':'芯片型号','uptimeSeconds':'运行时长（秒）','heapTotalBytes':'内部堆总量（字节）','heapFreeBytes':'内部堆可用（字节）','heapPeakUsedBytes':'内部堆峰值（字节）','heapMinFreeBytes':'内部堆最低可用（字节）','psramTotalBytes':'PSRAM 总量（字节）','psramFreeBytes':'PSRAM 可用（字节）','cfg':'设备配置','runtime':'运行状态','net':'网络','schedule':'调度','nextRun':'下次执行','last':'最近记录','date':'日期','message':'说明','device':'设备名称','wifiSsid':'已保存网络','state':'连接状态','deviceTime':'设备时间','timeSynced':'已校时','checkinAt':'计划时间','windowEnd':'窗口结束','windowStart':'窗口开始','autoDaily':'自动打卡','jitterSeconds':'随机延迟（秒）','on':'启用','gap':'去重间隔（秒）','quota':'每日上限','channels':'附加通道','accounts':'账号','slots':'微信槽位','present':'已保存','expiresIn':'剩余有效秒数','rssi':'信号强度','accountGap':'账号间隔（秒）'}
        rows=[]
        if isinstance(data,dict):
            for key,value in data.items():
                title=(prefix+' / ' if prefix else '')+labels.get(key,key)
                if isinstance(value,(dict,list)):rows.extend(self.flatten(value,title))
                else:rows.append((title,('是' if value else '否') if isinstance(value,bool) else value))
        elif isinstance(data,list):
            for i,value in enumerate(data):rows.extend(self.flatten(value,f'{prefix} [{i}]'))
        return rows

    def display_result(self,message):
        data=message.get('data')
        if message.get('evt')=='error':self.show_rows(('操作结果','说明'),[('设备拒绝',message.get('msg','失败'))]);return
        if isinstance(data,dict) and isinstance(data.get('aps'),list):
            enc={0:'开放',1:'WEP',2:'WPA',3:'WPA2',4:'WPA/WPA2',5:'企业认证',6:'WPA3',7:'WPA2/WPA3'}
            self.show_rows(('序号','SSID','RSSI','频道','加密','BSSID'),[(i,a.get('ssid') or '隐藏网络',a.get('rssi'),a.get('channel'),enc.get(a.get('enc'),str(a.get('enc'))),a.get('bssid')) for i,a in enumerate(data['aps'])])
            self.result_meta.set(f"扫描到 {len(data['aps'])} 个接入点 · 同名 SSID 不去重，可滚动查看全部")
        elif isinstance(data,dict) and isinstance(data.get('accounts'),list) and message.get('_operation') in ('test.login','test.chain','test.checkin','test.multi'):
            self.show_rows(('账号','操作','结果','说明'),[(a.get('label'),a.get('operation'),'跳过' if a.get('status')=='skipped' else '通过' if a.get('ok') else '未通过',a.get('msg')) for a in data['accounts']])
        else:
            changes=[('已提交修改项','、'.join(self.changed_fields)),('其余字段','保留设备原记录')] if self.current_op=='account.patch' and getattr(self,'changed_fields',None) else []
            self.show_rows(('项目','结果'),[('结果',message.get('msg','已确认'))]+changes+self.flatten(data))

    def update_accounts(self):
        self.account_table.delete(*self.account_table.get_children())
        slots={s.get('slot'):s for s in self.snapshot.get('wxSlots',[])}
        for row in self.accounts:
            wx=slots.get(row['slot'],{})
            self.account_table.insert('','end',iid=str(row['slot']),values=(row['slot'],'主账号' if row.get('primary') else row['label'],row.get('user','未提供'),'设备未提供' if 'passwordSet' not in row else '已配置' if row['passwordSet'] else '未配置','设备未提供' if 'fingerprintSet' not in row else '已配置' if row['fingerprintSet'] else '未配置','已保存' if wx.get('present') or wx.get('valid') else '未授权',notification_labels(self.snapshot.get('notification',{}),row['label']),'单人模式' if row.get('primary') else '☑ 启用' if row.get('enabled') else '☐ 停用',(row.get('lastDate','')+' '+row.get('lastMsg','')).strip() or '暂无记录'))
        self.account_hint.set(f"共 {len(self.accounts)} 个账号 · "+('账号快照已读取' if 'revision' in self.account_snapshot else '旧固件只提供打码摘要；完整学号与安全管理需升级固件'))

    def consume(self,message):
        data=message.get('data');msg=message.get('msg','')
        if msg=='status' and isinstance(data,dict):
            self.snapshot.update(data);cfg=data.get('cfg',{});net=data.get('net',{});run=data.get('runtime',{});schedule=cfg.get('schedule',{})
            rows=[('设备名称',cfg.get('device','—'),'当前设备'),('固件版本',data.get('fw','—'),''),('已连接网络',net.get('ssid') or '未连接',''),('IP 地址',net.get('ip','—'),''),('设备时间',net.get('deviceTime','—'),''),('自动打卡','开启' if schedule.get('autoDaily') else '关闭',''),('计划时间',schedule.get('checkinAt','—'),''),('下次执行',data.get('schedule',{}).get('nextRun','—'),''),('最近结果',data.get('last',{}).get('message','暂无'),'')]
            for key,title in [('heapPeakUsedBytes','内部堆峰值'),('heapFreeBytes','可用内部堆'),('psramFreeBytes','可用 PSRAM')]:rows.append((title,str(run[key])+' B' if key in run else '设备未提供',''))
            self.overview_table.delete(*self.overview_table.get_children())
            for row in rows:self.overview_table.insert('','end',values=row)
            self.updated.set('最近读取 '+datetime.datetime.now().strftime('%H:%M:%S'))
        elif msg=='account snapshot' and isinstance(data,dict):
            changed=self.account_snapshot.get('revision')!=data.get('revision')
            self.account_snapshot=data;self.accounts=snapshot_accounts(data)
            if changed:
                self.cancel_qr();self.target=None;self.target_revision=None
                for key in ('openid','access','refresh'):
                    if key in self.state['vars']:self.state['vars'][key].set('')
                if hasattr(self,'resolved'):self.resolved.set('账号表已刷新，请重新匹配目标')
                self.rebuild_targets()
        elif msg=='accounts' and isinstance(data,list):self.accounts=[dict(a,slot=i) for i,a in enumerate(data)]
        elif msg=='wx credentials' and isinstance(data,dict):self.snapshot['wxSlots']=data.get('slots',[])
        elif msg=='notify' and isinstance(data,dict):
            self.snapshot['notification']=dict(data,style={'get':0,'post_text':1,'post_json':2}.get(data.get('style'),2))
        self.update_accounts()
        if not self.state.get('dirty'):
            for key,value in device_defaults(self.current_spec()[1],self.snapshot).items():
                if key in self.state['vars']:self.state['vars'][key].set(value)
            self.state['dirty']=False
        if message.get('_operation')==self.current_op and self.current_op:self.display_result(message)
        if message.get('evt')=='progress' and self.current_op in ('test.login','test.chain','test.checkin','test.multi'):
            account=message.get('account','设备');self.result_progress[account]=message.get('stage','处理中')
            self.show_rows(('目标账号','阶段'),list(self.result_progress.items()))

    def reset_device(self):
        self.cancel_qr();self.snapshot={};self.account_snapshot={};self.accounts=[];self.target=None
        self.overview_table.delete(*self.overview_table.get_children());self.update_accounts()
        self.updated.set('设备已断开，状态已清空');self.clear_log()
        self.show_rows(('项目','结果'),[('状态','设备已断开，未完成命令不会自动重放')])
        self.render_operation()

    def close(self):
        if self.match_timer:self.root.after_cancel(self.match_timer);self.match_timer=None
        super().close()

    def poll(self):
        for _ in range(100):
            try:kind,value=self.events.get_nowait()
            except queue.Empty:break
            if kind=='log' and self.developer.get():
                self.log.configure(state='normal');self.log.insert('end',value+'\n')
                lines=int(self.log.index('end-1c').split('.')[0])
                if lines>500:self.log.delete('1.0',f'{lines-500}.0')
                self.log.see('end');self.log.configure(state='disabled')
            elif kind=='message':self.consume(value)
            elif kind=='disconnected':self.connected=False;self.reset_device();self.status.set('设备已断开')
            elif kind=='refresh_failed':self.result_meta.set('操作已确认，但刷新失败；请手动刷新')
            elif kind=='done':
                operation,result,error=value;self.busy=False
                if error:
                    self.status.set(f'操作失败 / 结果未知（{error}），请查询后再决定是否重试')
                    self.update_accounts()
                    if operation=='connect':self.connected=False;self.reset_device()
                else:
                    if operation=='scan':
                        self.devices=sorted(result,key=lambda x:-(x[2] or -999))
                        self.device['values']=[f'{a} | {b} | {c} dBm' for a,b,c,_d in self.devices]
                        if self.devices:self.device.current(0)
                        else:self.device.set('')
                    elif operation=='connect':self.connected=True
                    elif operation=='disconnect':self.connected=False;self.reset_device()
                    elif operation=='resolve':self.resolve_target();self.pending_resolve=False
                    self.status.set('设备已确认 · 控制就绪' if self.connected else f'未连接 · 发现 {len(self.devices)} 台设备')
        if not self.closing:self.root.after(80,self.poll)
