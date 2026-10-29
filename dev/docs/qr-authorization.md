# 蓝牙中控内置微信扫码：可行性与实现

## 当前验收更新

2026-10-29 用户确认本轮基本功能已查验无误，并授权 0.8.0 Latest。未提供逐项三 Cookie 闭环、逐板型 / 手机与长期兼容明细，不能把通用确认扩展为完整矩阵。下文开发阶段的失败、公开网络验证和测试次数保留为历史证据。当前操作先按标签 / 序号匹配目标，再扫码填表并手动保存。

## 结论与已验证边界

可以适配，已在 Android 和电脑 BLE GUI 实现，随 0.8.0 发行。使用用户指定的微信 QR Connect appid、scope、state 与应用宝回调地址，展示二维码、允许刷新 / 取消；扫码确认后从本次回调的三个 Cookie 提取 OpenID / Access Token / Refresh Token，填入当前导入表单。仍需用户核对账号槽位并主动发送 BLE，不自动打卡。

已直接验证公开二维码页面返回 HTTP 200，包含 `/connect/qrcode/<uuid>`；二维码图片返回 JPEG，电脑端 Pillow 成功解码。生产 Python 客户端的匿名长轮询收到 `wx_errcode=408`（等待扫码），没有授权字段。页面自身脚本确认成功时使用 405 / wx_code，并回跳到应用宝 OAuth 地址。无 code 的空回跳返回 JSON 参数错误，说明该回跳由服务端处理。

真实微信扫码涉及账号本人确认，本次未代替用户执行，因此**真实扫码后返回三 Cookie、实际回填及设备保存的完整闭环仍待用户验证**。现有三 Cookie 回调、HttpOnly Cookie 提取、失败、取消和迟到结果处理使用合成数据模拟测试。不能把“公开二维码与等待状态正常”描述为真实授权已验收；遇到服务端行为变化时保留手动导入。

用户已确认此前电脑端与手机端 BLE 中控功能通过验收，未提供逐板型 / 手机 / 系统细节；该确认不扩展到本轮新扫码功能，也不代表独立桌面打卡程序已完成真实账号验证。

## 双端实现

- 共用流程：新建内存 HTTP 会话 → 请求官方二维码页并解析 UUID → 下载并显示 JPEG → 每次长轮询状态 → 405 后携 code 访问指定 OAuth 回调 → 仅三个 Cookie 完整时回填 → 关闭会话并清空临时 Cookie。
- 电脑：`tools/qr_authorization.py` 负责标准库 HTTP / CookieJar 与解析，`tools/qr_dialog.py` 提供 Tk 子窗口；Pillow 用于 JPEG 显示。`ble_gui.py` 仅在微信导入表单接入入口，字段仍通过既有 `wx.import` 保存。刷新时取消旧会话，使用代次标识拒绝旧结果，窗口限制用户同时修改主表单；切换表单、断开或退出清理扫码。
- Android：`data/QrAuthorization.kt` 使用 HttpURLConnection、独立 Cookie 映射、有界读取与严格 HTTPS 主机校验；`ui/QrAuthorizationPanel.kt` 展示二维码、刷新 / 取消和过程状态。回填使用当前表单内存 state，不使用 rememberSaveable 持久化令牌。新增 INTERNET 权限，但不运行学校登录或独立打卡；无扫码时 BLE 配置仍可离线运行。
- 不使用 WebView、内嵌浏览器配置目录、Javascript bridge 或系统浏览器 Cookie，不执行服务器返回的脚本；仅解析明确的扫码状态与 code 字段。HttpOnly 不阻止本次 HTTP 客户端读取服务端 Set-Cookie。
- `expiresAt` 填 0，让固件首次使用前刷新。不会把 expires_in 误当绝对 Unix 秒，也不让二维码页面继续保活已交给设备的凭据。

## 使用方式

电脑先安装 `python -m pip install -r tools/requirements-ble.txt`（bleak、Pillow），打开 BLE GUI 并连接本人设备。进入微信授权 → 导入本人授权 → 按标签或序号匹配目标 → 微信扫码获取授权。用本人微信确认后，弹窗关闭，字段回填；核对槽位后点击执行 / 保存。

Android 使用 0.8.0 Debug APK，进入微信导入表单，先匹配目标账号，再获取二维码。二维码可刷新，确认后填入表单；再点击核对并继续、确认发送设备。**手机的相机无法扫描同一屏幕，且敏感表单限制截图；需让二维码显示在另一台设备上，或改用电脑 BLE 中控显示。** 本实现不承诺单手机截图识别 / 相册分享 / 微信内直接确认。

网络失败、拒绝授权、缺少任一 Cookie、二维码过期或格式变化时不会填入不完整凭据。可刷新重试或手动填写。开始新扫码和切换槽位会清空此前输入的三项授权，避免旧凭据误入新槽位。取消 / 断线后未完成操作不自动重放；用户需重新核对目标账号和槽位。电脑和手机的网络独立于 ESP32 网络。

## 隐私与限制

仅允许微信公开登录主机和应用宝指定主机的 HTTPS；回调重定向不能离开应用宝主机。不忽略证书错误，不读取系统浏览器 / 微信的数据目录，不使用剪贴板。清空旧 Cookie 后才兑换本次 code，防止旧会话与新二维码混用。完整性和域校验失败时停止。

二维码、UUID、code 和三 Cookie 不写入日志、磁盘或备份；Android 禁用 HTTP 缓存，扫码面板继承现有敏感表单防截屏。Python 会话不配置磁盘 CookieJar；取消后正在等待的请求最长等待网络超时退出，迟到结果不可回填。令牌仍仅在当前表单内存保留，提交 / 关闭后沿用原清理策略。

服务依赖用户指定的第三方应用和回调，并非本项目自有微信应用；服务端、scope 或 Cookie 策略改变会导致功能失效。每位账号持有人必须自己确认授权，授权不会自动关联学校账号，仍需人工核对槽位。已有浏览器方式保持可用。

## 验证入口

```powershell
python -m unittest discover -s tools/tests -v
./android/build-local.ps1
python tools/check_project.py
python dev/tools/check_privacy.py --source
```

工具新增解析、HTTPS / 跳转白名单、三 Cookie 完整性 / 冲突、旧会话清除、取消和弹窗代次隔离回归。Android 新增解析、主机白名单、三字段完整性和模拟 HTTP 回调回归。设备端测试 APK 仅构建，未连接真实手机执行。

本轮未改动固件协议、根目录 VERSION 或已发布标签，不覆盖已有发行资产。开发测试 APK 路径为 `android/app/build/outputs/apk/debug/app-debug.apk`，保留本地原 Debug 签名，仅供本轮验证；正式发布时须分配新版本并重新构建。

参考：[用户指定微信二维码入口](https://open.weixin.qq.com/connect/qrconnect?appid=wxd44977328b36e647&redirect_uri=https%3A%2F%2Fyybadaccess.3g.qq.com%2Fpc_yyb%2Fpcyyb_oauth%3Flogin_type%3DWX&response_type=code&scope=snsapi_login%2Csnsapi_runtime_pcsdk&state=web&fast_login=1&self_redirect=true) · [Android 用户指南](../../android/README.md) · [电脑 BLE 指南](../../docs/ble-gui.md)。

最终验证：Python 工具 35 项、Android 46 项均通过；lintDebug、assembleDebug、assembleDebugAndroidTest 通过，源码与测试 APK 隐私检查无阻断项。最终 Android 复验曾遇宿主 Microsoft JDK 17.0.16 的 C2 原生崩溃，改用仅影响本地构建 JVM 的 `-XX:TieredStopAtLevel=1` 后完整通过。崩溃日志留在忽略目录，未纳入源码。首次复验中旧帮助文案断言失效，已改为验证新的联网扫码说明后重新完整通过。未运行设备端仪器测试或真实扫码确认。

## 扫码后跳转与弹窗裁切修复

用户截图显示旧弹窗底部按钮裁切，以及扫码后“授权服务跳转到未允许的地址”。旧窗口固定 430×560，未按字体 / 缩放和按钮实际高度分配空间；现改为统一背景、网格布局、按内容请求尺寸确定最小窗口，并允许调整大小。二维码区域随窗口缩放，底部按钮及多行状态独立预留空间；终止会话清空旧二维码，关闭时取消界面定时器。

原回跳处理在收到 OAuth 302 时继续校验并跟随完成页。服务端可能已在该 302 的 Set-Cookie 中签发三个凭据，此时外域完成页不属于导入所需的流程。现在电脑端使用实际 urllib handler 顺序验证：先收 Cookie，完整且有效即关闭响应并结束授权，不访问任何完成页；字段缺失 / 过期 / 冲突时仍阻止不允许的跳转。手机端同步在读取 302 头后检查凭据，齐全即结束，不放宽主机白名单。新的电脑错误只报告检测到的字段数量，不包含 URL、code 或 Cookie 值。

已在双击实际使用的 WANAN 环境（Python 3.12.15、Tk 8.6、Pillow 12.3.0）完成 38 项工具测试，含 1.0 / 1.5 / 2.0 Tk 缩放下按钮边界、真实 urllib handler 管线中的 302 + HttpOnly Cookie、完整 / 部分凭据及禁止访问完成页回归。Android 47 项测试、lint 与两个测试用途 APK 构建通过，含相同 302 完成 / 缺字段分支；源码与应用 APK 隐私检查通过。

以上复现和验证使用合成授权头，不借用截图里的二维码或真实 Cookie。截图本身无法确定实际回调头是否已经齐全；修复后仍需本人重新扫码确认完整闭环。若仍失败，只反馈新的字段数量和错误提示，不提供实际凭据。本次不修改已发布标签或 GitHub 资产。
