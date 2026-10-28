# MoonCheck Desktop 1.1.0

Windows / Linux 原生桌面程序，CLI 与 tkinter 图形界面共用业务逻辑。电脑独立执行 CAS 登录、门户预热、微信授权刷新、MMTLS 获取小程序 code、平台取票、提交与结果复查，**无需 ESP32**。

1.1.0 已完成代码移植、离线回归及 Windows 打包；尚未用真实桌面账号验证取票/打卡，不能沿用固件或 APK 的实测结论。固件和 Android 当前仍为独立的 0.7.0，已有 GitHub 发行不变。电脑端以 [desktop-v1.1.0 预发行版](https://github.com/richard297779/UESTC-MoonCheck/releases/tag/desktop-v1.1.0) 独立提供 Windows ZIP，并附带电脑 BLE GUI 源码包。桌面 1.0.0 不支持平台微信票据要求，请升级。

## 使用前准备

- 本人 CAS 学号、密码、已信任浏览器指纹；获取方式见[快速开始](../docs/quickstart.md)。
- 每个账号自己的微信 openid、access、refresh。初始授权与固件一样由本人按[微信授权说明](../docs/quickstart.md#22-准备微信授权凭据)扫码准备；本版不接管或自动操作微信客户端。
- 宿舍现场确认的坐标、可访问校园打卡服务的网络。不要同时在桌面、ESP32、QR Connect 页面刷新同一套授权；导入前关闭授权页面。
- 自动运行需要电脑开机、联网且未休眠；程序不会自动安装系统计划任务。

## 图形界面

双击 `MoonCheck.exe`，首次运行在程序旁生成 `config.json`；建议放在本人可写、受保护的目录。

1. “基本”页填写本人 CAS 凭据、指纹和位置，点击保存配置。
2. 多人使用时先在“账号”页新增账号并保存。账号表为空时使用单人 `self`，最多 6 个账号。
3. 打开“微信授权”，选择**授权所属账号**，填写 OpenID / Access token / Refresh token / 绝对到期 Unix 秒；不知道到期时间可填 `0`，首次使用自动刷新。点击“导入并保存”立即落盘，敏感字段不回填到界面。
4. 点击“取票测试”。该操作只刷新授权、获取两次独立小程序 code、检查绑定并申请票据，**不会提交打卡或发送通知**。
5. 点击登录、查状态，窗口内确认后执行打卡。通知页配置各账号的推送通道。

授权刷新后会立即保存旋转凭据；即使后续步骤失败，界面也会回读新凭据。更改学号时清空该账号旧微信授权和会话，需要为新身份重新导入。密码/指纹仍应自行核对。

“清除该账号授权”仅清除选中账号；导出配置会递归清空学号、位置与地址、历史消息、密码、指纹、会话、微信凭据、票据和通知 URL；导出副本不是可直接运行的账号备份，需要本人重新配置。HTTP 调试输出隐藏 URL 路径、查询参数和重定向票据，`config show --all` 兼容参数也保持脱敏。真实配置包含明文凭据，请勿上传、公开日志或同步到共享网盘。

## 命令行

```text
MoonCheck.exe --version
MoonCheck.exe config set student_id 本人学号
MoonCheck.exe config set password
MoonCheck.exe config set browser_fingerprint 本人指纹
MoonCheck.exe wx import                 # 逐项隐藏输入；到期时间回车为 0
MoonCheck.exe wx status                 # 仅显示是否保存和到期时间
MoonCheck.exe wx test                   # 只验证取票，不提交
MoonCheck.exe login
MoonCheck.exe status
MoonCheck.exe checkin --dry-run         # 完全离线，仅展示拟提交载荷
MoonCheck.exe checkin                   # 真实提交，需在窗口内
```

多人模式使用 `wx import -a A01` / `wx test -a A01` / `wx clear -a A01`。单人也可显式指定 `-a self`。账号、通知和计划命令与 1.0.0 保持兼容：

```text
MoonCheck.exe account add A01 本人学号 - 本人指纹
MoonCheck.exe sched set 22:00 23:30
MoonCheck.exe notify list
MoonCheck.exe auto                      # 单轮，逐账号执行与通知汇总
```

也可使用 `wx import --file 本人授权.json`，避免将 token 放入进程参数和历史。文件格式：

```json
{
  "openid": "本人 OpenID",
  "access": "本人 access token",
  "refresh": "本人 refresh token",
  "expires_at": 0
}
```

`expires_at` 是绝对 Unix 秒，不能直接填 Cookie 的 `expires_in` 相对秒数；可直接填 `0`。导入文件也属于敏感文件，使用后自行妥善处理，不提交仓库。

退出码：0 成功，1 警告/窗口外/配置被其他实例占用，2 失败/未配置，3 CAS 二次验证。`--force` 仍可显式越过本地窗口和已打卡检查，不跳过微信取票及服务端校验。

## 自动任务与失败处理

沿用系统调度器调用 `MoonCheck auto` 的方式；例如每天 22:02 启动，窗口内每 3 分钟重试到 23:30。此处只是配置指南，不会替你创建任务。时间窗口和提交 clientTime 使用中国标准时间 UTC+8。

- 每次真正提交前获取新票据，两个 code 不复用；票据和 MMTLS 密钥只在内存中保留。
- 授权过期先刷新；login_buffer 明确被拒绝时最多刷新后重试一次。
- 返回 46001/46002/46003 或网络结果未知时，本轮停止，不自动重放提交；下一次 `auto` 先查今日结果，再决定是否重新取票提交。
- 同配置的 CLI/GUI 网络操作使用非阻塞文件锁，避免同时旋转 token。不要在多台设备上共用同一授权。
- 未导入该账号微信授权时拒绝提交；不借用其他账号或顶层授权。

## 从源码运行与构建

Python 3.10+（tkinter），运行依赖为 PyCryptodome；旧“零第三方依赖”描述仅适用于桌面 1.0.0。

```powershell
# 在 desktop/ 执行；建议独立虚拟环境
python -m pip install -r requirements-build.txt
python mooncheck.py --version
python -m unittest discover -s tests -v
python mooncheck.py                     # 图形界面
./build-local.ps1                       # Windows：测试并打包
```

也可使用现有 `build_windows.bat`（conda 环境需预装 requirements-build.txt）。Linux 在目标 Linux/WSL 环境执行 `bash build_linux.sh`，PyInstaller 不交叉编译；Linux 二进制本次未构建/实测。

Windows 产物 `dist/windows/MoonCheck.exe`；分享包 `dist/packages/MoonCheck-Desktop-1.1.0-windows.zip`、manifest 和 SHA-256 由构建脚本自动生成。也可在构建环境单独执行 `python package_local.py` 重新核验与打包。单文件程序内含加密依赖，使用者不用安装 Python。新构建不会混入历史固件发行包，也不会自动推送 GitHub。

文档：[移植计划](../dev/docs/desktop-port-plan.md) · [架构](../dev/docs/desktop-app.md) · [验证记录](docs/verification.md) · [第三方声明](THIRD_PARTY.md)
