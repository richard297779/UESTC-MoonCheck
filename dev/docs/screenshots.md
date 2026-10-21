# 配图清单（维护用）

用户文档 [`../../docs/quickstart.md`](../../docs/quickstart.md) 里引用的截图，文件放在
[`../../docs/assets/quickstart/`](../../docs/assets/quickstart/)。

| 文件名 | 内容 | 状态 |
| --- | --- | --- |
| `01-pip-install-bleak.png` | `pip install bleak` 成功 | 缺 |
| `02-power-5v.png` | 板子供电方式（USB / 5V / 接线） | 缺 |
| `03-login-jzsz.png` | Chrome 已登录 jzsz.uestc.edu.cn | 有 |
| `04-cookie-fingerprint.png` | Application → Cookies → idas 域的 `MULTIFACTOR_BROWSER_FINGERPRINT` | 有 |
| `05-bfp-info-url.png` | 备用取法：Network 里的 `bfp/info?bfp=…` | 有 |
| `06-h5-page.png` | 宿舍里打开晚安成电，能看到地图与「请打卡」 | 有 |
| `07-reverse-coords.png` | Network 过滤 `reverse`，Request URL 里的 lat/lng | 有 |
| `08-ble-connect.png` | `--list` 结果与连接成功提示 | 有 |
| `09-config-commands.png` | wifi/auth/loc/sched/auto 全部回 ok | 缺 |
| `10-chain-ok.png` | `chain 0` 回执，`appEntry: true` | 有 |
| `11-status-done.png` | `status` 回执，`last.ok: true` | 缺 |

补图时注意：

- 截图前打码：指纹、学号、密码、Cookie、完整经纬度一律涂黑或马赛克；
- 文档正文统一用 `XXXX` 占位，不需要真实值；
- 新增图片后，在 `docs/quickstart.md` 里用相对路径 `assets/quickstart/<文件名>` 引用。
