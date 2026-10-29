"""A QR-only Tk dialog. Never reads cookies from an existing browser profile."""
import io
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk
from tkinter import font as tkfont

from qr_authorization import QrError, QrSession


class QrDialog:
    def __init__(self, parent, slot, receive):
        from PIL import Image, ImageTk
        self.Image, self.ImageTk = Image, ImageTk
        self.receive = receive
        self.events = queue.Queue()
        self.generation = 0
        self.closed = False
        self.session = None
        self._bitmap = None
        self._poll_after = None
        self.window = tk.Toplevel(parent)
        self.window.withdraw()
        self.window.title('微信扫码授权 · 槽位 ' + slot)
        self.window.resizable(True, True)
        self.window.transient(parent)
        self.window.configure(background='#f3f6fb')
        style = ttk.Style(self.window)
        style.configure('Qr.TFrame', background='#f3f6fb')
        style.configure('Qr.TLabel', background='#f3f6fb', foreground='#334155')
        style.configure('Qr.Title.TLabel', background='#f3f6fb', foreground='#142c49', font=('Microsoft YaHei UI', 16, 'bold'))
        style.configure('Qr.Image.TLabel', background='white')
        content = ttk.Frame(self.window, style='Qr.TFrame', padding=20)
        content.pack(fill='both', expand=True)
        content.columnconfigure(0, weight=1)
        content.rowconfigure(2, weight=1)
        self.status = tk.StringVar(value='正在获取微信二维码…')
        ttk.Label(content, text='本人微信扫码授权', style='Qr.Title.TLabel').grid(row=0, column=0, pady=(0, 8))
        ttk.Label(content, text='目标槽位 ' + slot + ' · 仅填入表单，不自动发送', style='Qr.TLabel').grid(row=1, column=0, pady=(0, 16))
        self.image_frame = tk.Frame(content, background='white', width=320, height=320, highlightthickness=1, highlightbackground='#dce4ef')
        self.image_frame.grid(row=2, column=0, sticky='nsew')
        self.image = ttk.Label(self.image_frame, style='Qr.Image.TLabel', anchor='center')
        self.image.place(relx=.5, rely=.5, anchor='center')
        line_height = tkfont.nametofont('TkDefaultFont').metrics('linespace')
        status_frame = ttk.Frame(content, style='Qr.TFrame', height=line_height * 3 + 12)
        status_frame.grid(row=3, column=0, sticky='ew', pady=(8, 0))
        status_frame.grid_propagate(False)
        status_frame.columnconfigure(0, weight=1)
        status_frame.rowconfigure(0, weight=1)
        self.status_label = ttk.Label(status_frame, textvariable=self.status, style='Qr.TLabel', wraplength=390, justify='center')
        self.status_label.grid(row=0, column=0)
        ttk.Label(content, text='请用本人微信扫描另一块屏幕上的二维码。\n确认后返回表单，核对账号与槽位再保存。', style='Qr.TLabel', justify='center').grid(row=4, column=0, pady=(0, 16))
        row = ttk.Frame(content, style='Qr.TFrame')
        row.grid(row=5, column=0, sticky='ew')
        row.columnconfigure((0, 1), weight=1)
        self.refresh_button = ttk.Button(row, text='刷新二维码', command=self.refresh)
        self.refresh_button.grid(row=0, column=0, sticky='ew', padx=(0, 6))
        self.cancel_button = ttk.Button(row, text='取消', command=self.close)
        self.cancel_button.grid(row=0, column=1, sticky='ew', padx=(6, 0))
        self.window.update_idletasks()
        width = max(460, self.window.winfo_reqwidth())
        height = max(640, self.window.winfo_reqheight())
        self.window.minsize(width, height)
        self.window.geometry(f'{width}x{height}')
        self.image_frame.bind('<Configure>', self._resize_image)
        self.window.deiconify()
        self.window.grab_set()
        self.window.protocol('WM_DELETE_WINDOW', self.close)
        self.refresh()
        self._poll_after = self.window.after(100, self.poll)

    def refresh(self):
        if self.session:
            self.session.close()
        self.generation += 1
        generation = self.generation
        self.session = session = QrSession()
        self.image.configure(image='')
        self.image.image = None
        self._bitmap = None
        self.status.set('正在获取新二维码…')
        def worker():
            try:
                self.events.put((generation, 'image', session.start()))
                deadline = time.monotonic() + 240
                while not session.cancelled.is_set() and time.monotonic() < deadline:
                    status, fields = session.poll()
                    if status == 405:
                        self.events.put((generation, 'fields', fields))
                        return
                    if status in (402, 403):
                        self.events.put((generation, 'status', '二维码已过期或授权被取消，请刷新'))
                        return
                    self.events.put((generation, 'status', '已扫码，请在微信确认授权' if status == 404 else '请使用本人微信扫码并确认授权'))
                    session.cancelled.wait(1)
                if not session.cancelled.is_set():
                    self.events.put((generation, 'status', '等待已超时，请刷新二维码'))
            except QrError as error:
                if not session.cancelled.is_set():
                    self.events.put((generation, 'status', str(error)))
            except Exception:
                self.events.put((generation, 'status', '授权服务暂不可用，可刷新或手动导入'))
            finally:
                session.close()
        threading.Thread(target=worker, daemon=True).start()

    def poll(self):
        if self.closed:
            return
        if self._poll_after is not None:
            self.window.after_cancel(self._poll_after)
        self._poll_after = None
        while not self.events.empty():
            generation, kind, value = self.events.get_nowait()
            if generation != self.generation or self.closed:
                continue
            if kind == 'image':
                try:
                    bitmap = self.Image.open(io.BytesIO(value))
                    if bitmap.width > 2048 or bitmap.height > 2048:
                        raise ValueError('oversized image')
                    bitmap.load()
                    self._bitmap = bitmap
                    self._resize_image()
                    self.status.set('请使用本人微信扫码并确认授权')
                except Exception:
                    self.status.set('二维码图片无法显示，请刷新')
                    self.session.close()
            elif kind == 'status':
                self.status.set(value)
                if self.session and self.session.cancelled.is_set():
                    self.image.configure(image='')
                    self.image.image = None
                    self._bitmap = None
            elif kind == 'fields':
                self.receive(value)
                self.close()
                return
        if not self.closed:
            self._poll_after = self.window.after(100, self.poll)

    def _resize_image(self, _event=None):
        if self.closed or self._bitmap is None:
            return
        limit = max(100, min(self.image_frame.winfo_width() - 24, self.image_frame.winfo_height() - 24, 360))
        bitmap = self._bitmap.copy()
        bitmap.thumbnail((limit, limit), self.Image.Resampling.NEAREST)
        photo = self.ImageTk.PhotoImage(bitmap)
        self.image.configure(image=photo)
        self.image.image = photo
        self.status_label.configure(wraplength=max(200, self.window.winfo_width() - 60))

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self._poll_after is not None:
            self.window.after_cancel(self._poll_after)
            self._poll_after = None
        self.generation += 1
        if self.session:
            self.session.close()
        while not self.events.empty():
            self.events.get_nowait()
        self.image.configure(image='')
        self.image.image = None
        self._bitmap = None
        self.window.grab_release()
        self.window.destroy()
