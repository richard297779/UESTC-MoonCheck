<!--
SPDX-License-Identifier: MIT
SPDX-FileCopyrightText: 2026 richard297779
Source: https://github.com/muyuhuanghun/MoonCheck-WX/blob/de5b09b108d20272281bf39371524fa25db3f6f4/docs/TECHNICAL-REPORT.md
-->

> **转载说明（来源 · 许可 · 修改记录）**
>
> 本文档非本仓库原创，转载自合作者公开仓库 **[muyuhuanghun/MoonCheck-WX](https://github.com/muyuhuanghun/MoonCheck-WX)** 的
> [`docs/TECHNICAL-REPORT.md`](https://github.com/muyuhuanghun/MoonCheck-WX/blob/de5b09b108d20272281bf39371524fa25db3f6f4/docs/TECHNICAL-REPORT.md)，
> 按原仓库许可条款保留署名与许可证副本后收录。
>
> | 项目 | 内容 |
> | --- | --- |
> | 作者署名 | 正文署名「**MoonCheck-WX 项目组**」（上游仓库由 [@muyuhuanghun](https://github.com/muyuhuanghun) 维护），原样保留、未移除任何归属信息 |
> | 固定版本 | commit [`de5b09b`](https://github.com/muyuhuanghun/MoonCheck-WX/commit/de5b09b108d20272281bf39371524fa25db3f6f4)（2026-10-24，`docs: MoonCheck-WX 独立仓库 README 与技术报告`）；git blob `622bf4a6a752f531f413c364bde3d3880075fe3c` |
> | 许可条款 | 上游仓库采用 **MIT License**，版权声明 `Copyright (c) 2026 richard297779`，与本仓库根目录 [LICENSE](../LICENSE) 完全一致；按 MIT 要求，许可全文随本副本保留（见下方折叠块） |
> | 修改情况 | 2026-10-25 转载时以 blob 哈希核对：正文与上述上游版本**逐字节一致**；本仓库仅在文件顶部追加本说明块与 SPDX 注释，未改动原文任何内容 |
> | 后续维护 | 上游如有更新，以固定版本 permalink 为准同步或修订本说明；对文档内容的意见请反馈至上游仓库 [Issues](https://github.com/muyuhuanghun/MoonCheck-WX/issues) |
>
> <details>
> <summary>MIT 许可证全文（随副本保留）</summary>
>
> ```text
> MIT License
>
> Copyright (c) 2026 richard297779
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in all
> copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
> SOFTWARE.
> ```
>
> </details>

---

# MoonCheck-WX 技术报告：基于内置微信协议栈的 ESP32 全自主校园打卡系统

**MoonCheck-WX 项目组**

2026 年 10 月 24 日

---

## 摘要

2026 年 10 月 23 日，电子科技大学「晚安成电」打卡平台在其 H5 前端引入操作票据机制：打卡提交接口 `POST /api/student-checkin-record/checkin` 强制携带由微信小程序运行时签发的 `X-Checkin-Ticket`，而票据源头是仅能由小程序运行时产生的 `wx.login` code。该变更使包括本项目原始实现在内的所有非微信客户端方案失效。

本报告提出并实现 MoonCheck-WX：一种使 ESP32 单片机完全脱离手机、电脑与云端中转，独立完成「微信凭据维持、登录 code 生成、小程序会话建立、操作票据换取、打卡提交、结果复查」全链路的嵌入式系统方案。本方案的关键洞察是：票据的源头 code 并不必然由微信小程序运行时产生。通过腾讯应用宝（YYB）PC 端的开放协议，任何持有用户本人扫码授权凭据的设备，均可为任意目标小程序生成与官方运行时同类别的真实 code。

我们在 ESP32-S3 上完整移植了该协议栈（约 3,200 行 C++，含 MMTLS over 裸 TCP、混合 ECDH 加密、纯软件 AES-192-GCM），并配合学校侧 `check-bind` / `checkin-ticket` 适配实现端到端闭环。离线黄金向量自检 25 项全部通过与 Python 参考实现逐字节一致；连续取票压测中设备端全链成功率 5/6；2026-10-24 21:20:38，设备在没有手机、电脑参与的条件下完成全自主打卡（服务端确认距离 77.1 m），调度、幂等保护与失败自愈机制均按设计工作。全部代码已开源提交至上游仓库（PR #5）。

## 1 引言

### 1.1 问题定义

「晚安成电」为学生晚间打卡系统：学生需在每日 21:00–23:30 窗口内，于宿舍围栏内提交位置完成签到。项目此前的实现（UESTC-MoonCheck，ESP32 固件）通过统一身份认证（CAS）登录获取会话后直接调用提交接口。2026-10-23 16:39 平台前端更新后，提交接口返回业务码 46001：

```json
{"code": 46001, "msg": "请从微信小程序重新进入打卡应用后提交"}
```

经对平台前端与小程序发布包（v51 至 v56）的静态分析确认，新授权链为：

1. 微信运行时执行 `wx.login` 取得一次性 code（5 分钟有效）；
2. `POST /platform-app/api/auth/check-bind`，以 code 换取小程序 SESSION；
3. `POST /platform-app/api/auth/checkin-ticket`，以另一个 code 加小程序 SESSION 换取 43 字符操作票据（实测有效期约 30 分钟）；
4. 打卡提交请求携带请求头 `X-Checkin-Ticket`。

关键约束在于：`wx.login` code 只能由微信运行时产生。既有研究普遍据此推断设备端自主打卡不可行，社区方案退化为手机抓票中转形态。

### 1.2 核心洞察

我们的出发点是一个被普遍忽略的事实：腾讯应用宝（YingYongBao）PC 端本身就是腾讯官方运营的非微信运行时。其开放协议（scope 为 `snsapi_runtime_pcsdk`）允许持票设备代替微信运行时为任意小程序申请登录 code。该协议具备三个对本场景至关重要的性质：

1. 身份来源于用户本人：凭据由用户微信扫码授权产生，不含伪造身份；
2. 可长期维持：`refreshtoken` 机制支持无人值守的凭据续期（实测约每 2 小时轮换）；
3. 协议可嵌入式实现：传输层为 MMTLS over 裸 TCP，密码学原语（P-256 ECDH、HKDF-SHA256、AES-GCM、LZ4）均为 ESP32 mbedtls 可承载的公开算法，且协议中不存在私有签名秘密（唯一的接口签名键为应用宝网页端公开发布的防篡改键，可在多个开源项目中交叉验证）。

### 1.3 贡献

本工作的贡献包括以下四点：

- 协议工程：将 YYB 开放协议（QR Connect 授权、凭据刷新、login_buffer、MMTLS ManualAuth、0-RTT js-login）完整移植至 ESP32，约 3,200 行 C++，并以 25 项黄金向量实现与参考实现（YYB-Go-Enhanced、moon-uestc）的逐字节等价性验证；
- 密码学工程：识别并绕过 Arduino 预编译 mbedtls 的 AES-192 硬件限制，实现纯软件 AES-192-GCM（遵循 FIPS-197 与 NIST SP 800-38D），修复两处典型实现错误；
- 系统设计：面向每日无人值守场景的票据生命周期管理（属主绑定、25 分钟时效、失效自动重取、重启自愈）与集中式调度流程（取号、预热、打卡）；
- 实证闭环：2026-10-24 完成端到端实机验证，全自主打卡成功，全部代码以 PR 形式开源。

## 2 系统架构

### 2.1 授权链总体流程

首次配置仅需用户参与一次：终端显示二维码（open.weixin.qq.com 的 QR Connect，应用宝 appid），本人微信扫码确认后取得 `openid`、`accesstoken`、`refreshtoken`，通过 BLE 指令导入设备 NVS（每账号独立槽位）。

此后每日流程全部在 ESP32 上执行：

1. **T0，窗口起点（21:00）取号**：按需刷新凭据；拉取 login_buffer；完成 MMTLS 握手与 ManualAuth；执行两次 js-login 分别取得 code#1 与 code#2；以 code#1 经 check-bind 建立小程序 SESSION，以 code#2 经 checkin-ticket 换取操作票据（43 字符）。
2. **T0+5 分钟（21:05）预热**：CAS 登录建立 H5 会话，门户预热，进入微应用。
3. **T1（21:20）打卡**：持票据提交 `POST /api/student-checkin-record/checkin`，随后复查 `today` 接口确认结果。
4. **失败处理**：每 3 分钟重试直至窗口关闭；收到 46001/46002/46003 时清除票据并于下轮重取。

### 2.2 固件模块划分

| 模块 | 规模 | 职责 |
| --- | --- | --- |
| `wx_crypto` | 约 490 行 | varint 与 protobuf 字段、LZ4 block、SHA-256/HMAC/HKDF、AES-GCM（mbedtls 封装及 YYB blob 布局）、ECDH P-256、MD5 |
| `wx_aesgcm` | 约 230 行 | 纯软件 AES-192-GCM（见 3.3 节） |
| `wx_wire` | 约 600 行 | MMTLS 记录帧、握手、密钥派生、WPKG 头、ManualAuth、js-login 编解码 |
| `wx_dns` | 约 230 行 | HTTPDNS 服务发现、明文 HTTP/1.0 客户端 |
| `wx_mmtls` | 约 530 行 | 连接编排：握手、ManualAuth、0-RTT 取码，多候选时间预算 |
| `wx_oauth` | 约 180 行 | 应用宝 HTTPS：凭据刷新（轮换保存）、login_buffer 签名请求 |
| `wx_credential_store` | 约 110 行 | 每账号凭据 NVS 持久化 |
| `wx_selftest` | 约 240 行 | 开机黄金向量自检（25 项） |
| 学校侧适配 | 约 200 行增改 | check-bind、checkin-ticket、票据生命周期、调度接入 |

### 2.3 与既有系统的关系

本方案不替换既有 CAS 登录与打卡提交代码，而是在其上插入票据层。H5 会话（CAS 产生）与操作票据（微信链产生）是两套独立授权，提交时同时携带 `Cookie: SESSION=<H5 会话>` 与 `X-Checkin-Ticket: <票据>`。这与真实浏览器场景（小程序传票给 H5 页面、H5 页面持自己的会话提交）在服务端视角完全同构。

## 3 关键技术

### 3.1 协议选型与身份合法性

社区既有实现依赖常驻微信客户端（手机中转、云函数抓票），其设备授权实质是外部中转。我们要求授权链的每一环都在设备上执行。YYB 协议链中唯一的应用侧常量是接口签名键 `wgrdg373hy26ww2`，它不是密码学秘密，而是应用宝网页端公开发布的防篡改键（签名为 `md5(body + timestamp + key + nonce)`），在多个开源项目中可交叉验证。协议层呈现的 `WindowsxWebPlugin` 等客户端标识为第三方微信客户端形态的公开事实，而非身份伪造。用户以本人微信扫码完成 QR Connect 授权，与在 PC 上用微信登录应用宝的用户行为完全一致。

### 3.2 MMTLS 的嵌入式移植

MMTLS 为微信自研 TLS 变体：ECDHE P-256 握手、HKDF-SHA256 密钥派生（自定义 label 拼接而非标准 TLS 1.3 编码）、AES-GCM 记录层（AAD 为序列号、内容类型、版本 0xF103 与密文长度的级联）、0-RTT PSK 恢复。移植过程中确认了三个关键陷阱及相应修复：

1. **transcript 完整性**：握手哈希必须覆盖完整记录载荷（含 4 字节握手消息头）。C++ 实现中 `splitHandshake` 的自引用覆盖曾导致 transcript 缺失消息头，ServerFinished 校验失败、服务器以 alert 拒绝。该问题由实机对拍暴露，黄金向量无法覆盖此类交互缺陷。
2. **一次性票据语义**：login_buffer 为一次性登录票据，每次取码必须重新拉取；将同一致力于第二次 ManualAuth 会被静默拒绝。
3. **0-RTT 响应歧义**：短连接响应的 transcript 拼接顺序存在三种合法排列，叠加三个候选序列号，解密试探必须完整枚举（与参考实现一致）。

### 3.3 纯软件 AES-192-GCM

ManualAuth 混合加密固定使用 `SHA256(ECDH)` 的前 24 字节作为 AES 密钥。实测发现 Arduino-esp32 预编译 mbedtls 启用硬件 AES（`CONFIG_MBEDTLS_HARDWARE_AES`）后，`mbedtls_gcm_setkey` 对 192 位密钥返回错误 `-114`（`MBEDTLS_ERR_AES_INVALID_KEYLENGTH`），即该发行版仅支持 128 位密钥。

规避方案为独立实现 AES-192-GCM（模块 `wx_aesgcm`，仅正向加密、GHASH 与 CTR，约 230 行），要点如下：

1. GCM 解密仅需正向密码，无需实现解密轮；
2. 按密钥长度路由：128/256 位继续使用 mbedtls 硬件加速，192 位走软实现；
3. 实现过程中修正两处经典错误：其一，GHASH 的输入必须为密文而非明文（NIST SP 800-38D 第 7.1 节）；其二，解密方向的 GHASH 输入是接收到的密文而非 CTR 输出。两处错误均被外部测试向量（cryptography 库生成）捕获。

### 3.4 票据生命周期管理

| 设计点 | 机制 | 实测依据 |
| --- | --- | --- |
| 有效期 | 不依赖长期有效：每次打卡现场取新票（约 10 秒），取完即用 | 签发响应显示约 30 分钟 |
| 属主 | 票据绑定学号，多账号切换时属主不匹配自动重取 | 四账号独立凭据槽位 |
| 失效恢复 | 46001/46002/46003（HTTP 200 业务码与 403 承载两种情况）触发清票，下轮重取 | 平台错误码集合（前端代码取证） |
| 入口冲突 | `enterApp` 仅清除过期票据；URL 携带票据时才覆盖，否则小程序场景外会误清 | 2026-10-24 夜间实机发现并修复 |
| 重启自愈 | 票据存内存不持久化；重启后窗口内自动补打 | 调度器状态归零设计 |

### 3.5 会话隔离

系统全程维护三套相互隔离的凭据域：CAS/H5 会话（Cookie 罐，`platform: h5`）、小程序会话（显式 Cookie 头，`platform: mp-weixin`，存在期仅数秒）、微信凭据（NVS）。为此修复了 `HttpClient` 的一个既有缺陷：Cookie 罐无条件追加导致请求自带 Cookie 头时出现双 Cookie 头，服务端会话解析行为不确定。修复后请求自带 Cookie 头时罐子让位。

## 4 实验与评估

### 4.1 黄金向量对拍（离线）

以 Python 参考实现（moon-uestc，其本身与 YYB-Go-Enhanced 的 Go 参考逐字节一致）生成 16 组固定输入输出，另加 5 组 AES-192-GCM 向量（cryptography 库生成）与 4 项行为断言，共 25 项，开机自动执行。

| 类别 | 项数 | 结果 |
| --- | --- | --- |
| MMTLS 握手构造（ClientHello、PSK ClientHello） | 2 | 通过 |
| 密钥派生（master、handshake、app、finished、PSK one-way） | 6 | 通过 |
| 记录层加解密与 AAD 绑定 | 2 | 通过 |
| WPKG 头（含上游 Go 对拍样例回环） | 2 | 通过 |
| LZ4 编解码 | 3 | 通过 |
| js-login 明文、短连接包、记录帧 | 3 | 通过 |
| 传输信封构造与解密回环 | 2 | 通过 |
| AES-192-GCM（加密向量 3 项、解密回环、tag 篡改拒绝） | 5 | 通过 |
| 合计 | 25 | 全部通过 |

### 4.2 设备端取票链路稳定性

以 75 秒间隔连续触发设备端全链（凭据、login_buffer、MMTLS、取码、check-bind、换票，单轮约 10 秒）：

| 指标 | 数值 |
| --- | --- |
| 连续运行轮数 | 8（另有散测约 7 轮） |
| 端到端成功（签发 43 字符票据） | 8 轮中 5 次，其余为瞬时网络失败 |
| 失败模式 | 均为网络层瞬时失败，协议层零失败 |
| 单轮取票耗时 | 10.9 秒（含 HTTPDNS、握手、两次取码、学校接口两次） |
| 资源占用 | RAM 17.4%（56.5 KB / 320 KB），Flash 32.3%（S3 目标） |

### 4.3 端到端实战（2026-10-24）

| 时刻 | 事件 | 结果 |
| --- | --- | --- |
| 21:00:00 | 调度触发取号 | 10 秒完成票据安装 |
| 21:05–21:07 | 预热体检（四账号联检） | CAS 登录成功；检测到 A04 已由本人手动打卡（幂等输入） |
| 21:20:00 | 打卡触发 | 开始执行 |
| 21:20:26–36 | 现场取票 | 43 字符票据 |
| 21:20:38 | 持票提交 | 服务端接受，返回 `checked in at 2026-10-24 21:20:38 (distance 77.1 m)` |
| 21:20:39 | 复查 today | 确认入账 |
| 21:23–21:29 | 重试轮 | 幂等保护生效（报告 already done，不重复提交），聚合确认后重试自动终止 |

全程无崩溃、无重启、无人工介入。设备与手机热点直连，电脑关机状态下亦可运行（BLE 仅用于首次配置与诊断）。

### 4.4 失败自愈验证

| 场景 | 行为 | 验证方式 |
| --- | --- | --- |
| 票据被拒（4600x） | 清票，下轮重取 | 错误码路径单测，HTTP 200 与 403 双承载处理 |
| 网络瞬时失败 | 保留有效票据，重试续用 | 压测中 1 例 login_buffer 失败后自动恢复 |
| 凭据过期 | 临期自动刷新，轮换结果立即持久化 | 实测 refreshtoken 轮换 |
| 设备重启 | 调度状态归零，窗口内自动补打 | 多次烧录重启后行为确认 |

## 5 安全与伦理讨论

1. 身份合法性：整条授权链无伪造成分。用户本人扫码授权、微信真实签发 code、学校服务端正常验票。方案改变的是产生 code 的运行时，与用户在 PC 上微信登录应用宝的行为等价。
2. 凭据安全：微信凭据等价于设备级微信登录授权，仅存设备 NVS，刷新轮换即时落盘。仓库不含任何账号材料。设备遗失应视同微信授权泄露处理。
3. 使用边界：定位提交仍为设备配置的围栏坐标（与真实宿舍一致），未实现也不应实现位置伪造。方案目标是恢复既有的合法打卡自动化，而非绕过任何业务校验。
4. 风控提示：多账号同 IP 顺序登录的既有风控提示继续适用，账号间隔机制（25 秒）保留。

## 6 局限与未来工作

1. 协议维护风险：微信侧 MMTLS 改版将使取码失效。YYB 生态为多项目共担维护，跟进成本低于单点方案；应用宝凭据被用户主动撤销时需重新扫码。
2. 偶发崩溃：约 15 次运行出现 1 次延迟崩溃（成功完成后 30 至 90 秒，证据画像指向堆损坏）。因提交发生在取票后 1 至 2 秒、且重启自愈完善，暂不阻塞生产使用；已部署串口留痕，backtrace 到手后定点修复。
3. 跨日验证：当前完成单日闭环。跨日凭据轮换、微信掉线恢复、围栏与时段变更自适应需要更长时间的运行数据。
4. 多账号：每账号需其本人扫码一次；非本人代打不在本方案支持范围内（属主绑定机制天然阻止）。

## 7 结论

MoonCheck-WX 证明了一个此前被社区判定不可行的命题：在无手机、无电脑、无云端中转的条件下，ESP32 级别的微控制器可以独立完成依赖微信运行时的平台授权闭环。方法上，本工作的关键在于识别票据源头 code 可由用户本人授权的官方开放协议产生这一路径，并以逐字节等价性验证的工程方式将其移植到资源受限设备。2026-10-24 21:20:38 的实机打卡标志该系统从研究原型进入生产运行。全部代码已开源，供同系统高校社区复用与审计。

## 参考文献

1. richard297779/UESTC-MoonCheck：原始项目与 issue #3（平台票据机制报告）。https://github.com/richard297779/UESTC-MoonCheck
2. MoonCheck-WX 完整实现（本报告对应 PR）。https://github.com/richard297779/UESTC-MoonCheck/pull/5
3. MioYiSama/moon-uestc：同系统 Python/Go SDK 与 YYB 协议的 Python 移植（移植参考与黄金向量来源）。https://github.com/MioYiSama/moon-uestc
4. 525815266/YYB-Go-Enhanced：YYB 微信协议的 Go 参考实现（MMTLS、ManualAuth、js-login）。https://github.com/525815266/YYB-Go-Enhanced
5. NIST SP 800-38D：GCM 规范（GHASH 输入约定）。
6. FIPS-197：AES 规范（AES-192 密钥扩展）。
7. 微信官方文档：wx.login 与小程序登录凭证时效。https://developers.weixin.qq.com/miniprogram/dev/api/open-api/login/wx.login.html
