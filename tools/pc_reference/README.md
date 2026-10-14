# pc_reference · 打卡链路验证工具

> 完整方案与判定标准见 `docs/python-verification-plan.md`。本目录只放代码。

## 快速开始

```powershell
# 0) 安装 Python 3.12（若尚未安装，务必勾选 Add python.exe to PATH），然后：
python --version

# 1) 准备配置
cd tools\pc_reference
copy config.example.json config.json
notepad config.json        # 填 student_id / password / session_cookie / browser_fingerprint

# 2) 自检（不联网）
python verify.py selftest

# 3) V1：只读状态（零第三方依赖）
python verify.py status

# 4) V1：干跑看载荷，再真实打卡（每天一次）
python verify.py checkin --dry-run
python verify.py checkin

# 5) V2：脱离浏览器登录（需要 pycryptodome）
python -m pip install pycryptodome
python verify.py login
```

## 文件说明

| 文件 | 作用 | 对应固件模块 |
| --- | --- | --- |
| `verify.py` | 命令行入口（selftest / status / checkin / login / auto / probe-session） | 业务编排 |
| `wsc_http.py` | 最小 HTTP 客户端：手工 Cookie 罐、手工 302 跟随、零依赖 | `http_cli` |
| `wsc_cas.py` | CAS 登录链 + 密码 AES 加密（算法由抓包确认） | `cas_auth` |
| `wsc_checkin.py` | 晚安成电接口封装与安全字段提取 | `jzsz_api` |
| `config.example.json` | 配置模板（**复制成 config.json 后填写**） | `cfg_store` |

## 安全约定

- `config.json`、`out/`、`capture/raw/` 已在 `.gitignore` 中；**内含密码与一年期会话 Cookie，等同账号通行证**；
- 脚本不打印密码、Cookie 值、姓名与手机号；`today` 响应只提取安全字段；
- 频率纪律：登录实验每天 ≤ 3 次，打卡每天 1 次，只读探测 ≤ 5 次；
- 出现滑块、验证码、`abnormalFlag=true` → 立即停止并把响应发我。
