# 多账号顺序打卡设计（multi-account）

> 版本：v0.2（2026-10-18 去单人化）｜ 适用固件：`0.3.1`（**单一固件**，不再区分单人版/多人版；`-DWANAN_MULTIUSER` 编译宏已删除）
> 基础功能见 `docs/firmware-design.md`；本文只写**账号表与顺序打卡**相关的部分。

---

## 1. 目标与边界

| 项 | 结论 |
| --- | --- |
| 场景 | **一台设备服务多个账号**（室友/家人），到点后**依次**登录并打卡 |
| 为什么是"依次" | 设备只有一份 Cookie 罐与一条会话，同一时刻只能代表一个人；且并发提交是明显的机器特征 |
| 不做 | 不做多设备协同、不做云端排队；账号之间只靠"错峰间隔"降风险 |
| 上限 | **6 个账号**（`AccountStore::kMaxProfiles`），受打卡窗口总时长与 NVS 单值大小约束 |
| 单人 / 多人 | **同一条执行路径**：定时触发一律走 `MultiCheckin::runAll`。账号表里一条记录 = 单人；**表为空**时自动退回设备自身配置（`AppConfig::deviceProfile()` = `auth` + `loc` 那套 NVS 值），等价于「列表第一个用户」 |

---

## 2. 构建目标：只按板子分，不按人数分

| env | 板子 | Flash / 分区表 | 状态指示 |
| --- | --- | --- | --- |
| `esp32-s3-devkitc-1`（default_envs） | ESP32-S3-N16R8 | 16 MB / `partitions_16mb.csv` | WS2812 @ GPIO48 |
| `esp32-wroom-32` | 经典 ESP32-WROOM-32 | 4 MB / `partitions_4mb.csv`（方案 B，双槽 `0x1F0000`） | 普通 LED @ GPIO2 |

- 两个 env 同一份代码、同一个版本号 `FW_VERSION "0.5.0"`，不再有 `-multi` 后缀；
- 已删除：`WANAN_MULTIUSER` 编译宏、NVS 键 `multi_on`、`multiUserEnabled()/setMultiUserEnabled()`、`multi.set` 的 `enabled` 字段——**打几个人只由账号表决定**；
- 每个账号带独立开关 `CheckinProfile.enabled`：关掉某人 = 他不登录、不提交、**不进「全员成功」的分母**（`RunResult.disabledCount` 单独统计）；关掉所有人 = 今晚没有要打的人，本轮按"无事可做"落定，不会整夜重试。

---

## 3. 新增模块

### 3.1 `AccountStore`（`account_store.h/.cpp`）

NVS 命名空间 `accounts`，单键 `profiles` 存 JSON 数组：

```jsonc
[{ "l":"A01", "en":true, "u":"2023xxxxxxx", "p":"<密码>", "f":"<32位指纹>",
   "lat":30.75, "lng":103.93, "a":"地址", "d":"2026-10-14", "ok":true, "m":"checked in at …" }]
```

`en` = 该账号的生效开关，**缺省 true**（旧数据没有这个键，一律视为启用）。

| 方法 | 说明 |
| --- | --- |
| `addOrUpdate(profile)` | 按 `label` 去重覆盖；覆盖时**保留已有的打卡结果**（改密码不会误判"今天没打过"）；请求不带 `enabled` 时**沿用原开关**（防止改密码把关掉的账号偷偷打开） |
| `setEnabled(label, on)` / `enabledCount()` | 单人开关与生效人数统计 |
| `isCheckedInToday(label, date)` | 每人独立幂等 |
| `setResult(...)` | 每人独立记录结果 |
| `toJsonSummary()` | 脱敏：学号只留前 4 位，密码/指纹只报 `true/false`；每条带 `enabled` |
| 校验 | `label` 只允许 `A-Z a-z 0-9 - _`，1–12 字符；`user`/`pass` 必填；超过 6 个拒绝 |

### 3.2 `MultiCheckin`（`multi_checkin.h/.cpp`）

唯一入口 `runAll(force, probeOnly)` → `RunResult{total, enabledTotal, okCount, disabledCount, summary}`，**只在 net_task 中调用**。队列 = 账号表按 `enabled` 过滤；**表为空时队列 = `[deviceProfile()]`**（设备自己的 `auth` + `loc`）；关掉的账号记 `result="disabled"` 后跳过，不进 `enabledTotal`：

```
for 队列中每个账号 profile：
  1) 该账号今天已确认成功且 !force        → 记 already_done，跳过（不浪费登录次数）
  2) cookieJar.clear()                   → 换人必须换会话（指纹 Cookie 由 cas_auth 每次重新注入）
  3) checkinService.resetLoginState()    → 清零 30 分钟登录冷却（否则第 2 个账号必被挡住）★
  4) config.applyProfile(profile)        → 把该账号"套进"内存配置（不落盘）
  5) checkinService.perform(force, msg)  → 复用既有链路：登录→门户预热→体检→幂等→提交→复查
  6) accounts.setResult(...)             → 记录 + BLE 推送 evt.result{account,ok,result,msg}
  7) 还有下一个账号 → vTaskDelay(multiGapSeconds)  错峰
runAll 结束 → config.reload()            → 恢复设备自己保存的账号（status/hello 不会显示成最后一个）
```

★ 第 3 步是**多人正确性的关键**：`CheckinService` 的登录冷却是"按上一次登录"计算的，
不清零的话，账号 A 登录成功后账号 B 在 30 分钟内无法登录，会一路 `Pending` 到窗口结束。

---

## 4. 对既有代码的改动（共 4 处，全部向后兼容）

| 文件 | 改动 | 单人路径影响 |
| --- | --- | --- |
| `app_config.h/.cpp` | 新增 `applyProfile()` / `reload()` / `deviceProfile()` / `multiGapSeconds()` / `setMultiGapSeconds()`；NVS 新增 `multi_gap`（原 `multi_on` 与 `multiUserEnabled()/setMultiUserEnabled()` 已删除） | 无（间隔缺省 20 s） |
| `checkin_service.h/.cpp` | 新增 `resetLoginState()` | 无（单人不会调它） |
| `command_router.h/.cpp` | `begin()` 多两个依赖；新增 `account.*`（含 `account.enable`）/ `multi.set` / `test.multi` 指令；`Probe` / `DailyCheckin` / `MultiCheckin` **一律**转交 `workMultiCheckin`（不再有"多人开启"分支） | 定时路径统一；手动 `test.probe` / `test.checkin` 仍只作用于当前生效账号 |
| `main.cpp` | 增加 `g_accounts` / `g_multi` 并接线 | 无 |

---

## 5. 指令表（BLE / 控制台同名命令）

| op | 参数 | 说明 | 控制台 |
| --- | --- | --- | --- |
| `account.add` | `label,user,pass[,bfp][,lat,lng][,addr][,enabled]` | 新增/覆盖账号；省略坐标→沿用设备坐标；标签已存在且不带 `enabled`→沿用原开关 | `account add A01 2023xxx pass -` |
| `account.list` | — | 脱敏列表（每条带 `enabled`） | `account list` |
| `account.remove` | `label` | 删除 | `account rm A01` |
| `account.clear` | — | 清空 | `account clear` |
| `account.enable` | `label,on` | 单独开关某账号（`on=false` = 不登录、不提交、不进分母，汇总记 `result="disabled"`） | `account on A01` / `account off A01` |
| `multi.set` | `gap_s` | 账号之间间隔秒（默认 20，≤600）。**只剩这一个参数**；回执 `{"gapSeconds":N,"accounts":M,"enabled":K}` | `multi 30` |
| `test.multi` | `force?,probe?` | 立即按账号表跑一轮。**两个 flag 可组合**：`probe` = 只体检不提交（但仍会登录+预热，是完整演练）；`force` = 跳过"已打卡"检查，**窗口外也用它才允许登录** | `multi` / `multi probe` / `multi probe force` |

---

## 6. 时间预算（决定能不能塞进打卡窗口）

| 项 | 估算 |
| --- | --- |
| 单账号一轮（登录 5 请求 + 预热 + 入口 + `today` + 时段 + 打卡点 + 提交 + 复查） | 约 8–15 s（含 TLS 握手） |
| 账号间隔 `gap_s` | 默认 20 s |
| **6 个账号总计** | 约 **2.5–3.5 分钟** |
| 建议 | 多账号时把 `sched` 的打卡时间设在窗口早段（如 **21:05**），给失败重试留足余量；预登录体检（21:00）也会按生效账号各跑一次，第一个账号最先拿到授权 |

---

## 7. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 同 IP 短时间多账号登录被风控 | 中 | `gap_s` 错峰；每人坐标可不同；必要时把 `checkinAt` 拉开（多台设备各管一人是更稳的形态） |
| 某账号需要**验证码**（该账号没登记过信任浏览器） | 该账号失败，其余照常 | 单人失败隔离；`evt.result{ok:false,result:"need_mfa"}` + 告警；给该账号补 `bfp` |
| 微应用授权延迟（门户预热已解决主因） | 个别账号首轮 `Pending` | 调度器每 3 分钟重试；每人独立幂等，重试不会重复提交 |
| 密码明文存 NVS | 与单账号形态同级风险 | 不导出 NVS；`account.list` 已脱敏；必要时后续做 Secure Boot + NVS 加密 |
| 中途掉电 | 半轮未完成 | 每人结果即时落盘；重启后调度器重跑，已确认成功的账号被跳过 |
| 一次运行数十秒 | 看门狗？ | 全部在 net_task（10 KB 栈）内，HTTP 自身有延时让出 CPU；主循环 `loop()` 在 core 1 正常喂狗 |

---

## 8. 验收清单

- [ ] M1 `account add` ×2 → `account list` 显示 2 条且学号打码
- [ ] M2 `multi probe` → 两个账号分别给出 `session ok, today=PENDING, window …`，互不污染（第二个不报 4001）
      > ⚠️ 实现要点：`probe()` 本身是**只读体检、不会登录**。多人演练必须先
      > `ensureSession(force)` 建立会话，否则刚 `cookie.clear()` 之后的匿名会话必然被
      > 平台退回首页，报出误导性的 `warm-up did not help`（2026-10-14 实测踩到并已修）。
      > 窗口外演练要加 `force`：`multi probe force`。
- [ ] M3 `multi`（窗口内）→ 两个账号都 `CHECKED_IN`，平台端两条记录
- [ ] M4 再跑一次 `multi` → 两个都 `already_done`，**不再产生新登录**
- [ ] M5 关掉某个账号（`account off A02`）→ 该账号当天不再被尝试，其余照常；汇总记 `result="disabled"`，且**不进「全员成功」的分母**（否则永远达不到 allOk → 整夜重试）
- [ ] M6 中途拔电重启 → 已成功的账号被跳过，未成功的继续重试
- [ ] M7 启动后 `status` 的 `"account"` = **列表里第一个 enabled 账号**（setup 5b 自动套用）；表为空时是 `"device"`；`runAll` 结束 `reload()` 后不会停在最后一个账号

---

## 9. 迁移说明（旧"单人/多人两版" → 单一固件）

- 旧固件的 `multi_on`（config NVS）作废：升级后直接忽略，不必清理；
- 旧账号表条目没有 `en` 键 → 一律视为**启用**，行为不变；
- 以前靠 `multi off` 回到单人行为，现在等价做法是 `account clear`——清空后执行器自动用设备自身的 `auth` + `loc` 配置。
