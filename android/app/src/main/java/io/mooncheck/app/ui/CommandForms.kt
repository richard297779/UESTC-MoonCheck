package io.mooncheck.app.ui

import kotlinx.serialization.json.*

enum class Kind { Text, Secret, Int, Decimal, Choice }
data class Input(val key: String, val title: String, val hint: String = "", val initial: String = "",
    val kind: Kind = Kind.Text, val required: Boolean = true, val choices: List<String> = emptyList())
data class CommandForm(val title: String, val op: String, val description: String, val inputs: List<Input>,
    val step: String? = null, val confirm: Boolean = false, val extra: JsonObject = JsonObject(emptyMap()))

object Forms {
    val wifi = CommandForm("连接 WiFi", "wifi.set", "设备仅支持 2.4 GHz。配置保存后，设备会立即尝试连接。", listOf(
        Input("ssid", "WiFi 名称", "保留名称中的空格"), Input("pass", "WiFi 密码", "开放网络留空", kind = Kind.Secret, required = false)), "wifi")
    val auth = CommandForm("配置本人账号", "auth.set", "单人模式使用此配置。指纹来自你平时登录的浏览器，不是手机指纹识别。", listOf(
        Input("user", "学号"), Input("pass", "统一认证密码", kind = Kind.Secret),
        Input("bfp", "浏览器指纹", "32 位十六进制；留空保留设备原值", kind = Kind.Secret, required = false)), "auth")
    val wx = CommandForm("导入微信授权", "wx.import", "可内置微信扫码后填入三个字段，也可手动填写。扫码不会自动保存到设备；请核对本人账号和槽位，提交后清空输入。", listOf(
        Input("slot", "账号槽位", "单人默认 0；多人按账号页顺序核对", "0", Kind.Choice, choices = (0..5).map(Int::toString)),
        Input("openid", "OpenID", kind = Kind.Secret), Input("access", "Access Token", kind = Kind.Secret),
        Input("refresh", "Refresh Token", kind = Kind.Secret), Input("expiresAt", "到期 Unix 秒", "推荐填 0，由固件首次使用前刷新", "0", Kind.Int)), "wx", true)
    val location = CommandForm("设置宿舍坐标", "loc.set", "请使用本人在宿舍现场确认的坐标，纬度在前、经度在后。应用不会用手机位置覆盖设备。", listOf(
        Input("lat", "纬度", "例如 30.xxxxxx", kind = Kind.Decimal), Input("lng", "经度", "例如 103.xxxxxx", kind = Kind.Decimal), Input("addr", "地址说明", required = false)), "location")
    val schedule = CommandForm("设置自动计划", "sched.set", "使用设备时间。保存计划不会自动开启每日任务，完成配置和验证后请主动开启。", listOf(
        Input("at", "计划时间", "HH:MM", "22:00"), Input("window_end", "截止时间", "HH:MM", "23:30"), Input("jitter_s", "随机抖动（秒）", initial = "30", kind = Kind.Int)), "schedule")
    val account = CommandForm("添加或更新账号", "account.add", "最多 6 人，需本人同意。同名标签会覆盖该账号；经纬度需成对填写，都留空则沿用设备当前坐标（覆盖也如此）。地址可单独填写；保存后核对微信槽位。", listOf(
        Input("label", "账号标签", "1–12 位字母、数字、- 或 _"), Input("user", "学号"), Input("pass", "密码", kind = Kind.Secret),
        Input("bfp", "浏览器指纹", "可留空；覆盖时请重新确认授权", kind = Kind.Secret, required = false),
        Input("lat", "账号纬度", "与经度成对填写；都留空沿用设备坐标", kind = Kind.Decimal, required = false),
        Input("lng", "账号经度", "请填写本人现场确认的坐标", kind = Kind.Decimal, required = false),
        Input("addr", "账号地址", "可单独填写；留空不代表保留旧账号地址", required = false)), confirm = true)
    val accountCreate = account.copy(title="新建账号",description="新建须填写标签、学号、密码、32 位浏览器指纹；位置可选。需要支持 account-v2 的固件。",inputs=account.inputs.map { if(it.key=="bfp") it.copy(required=true,hint="32 位十六进制") else if(it.key=="addr") it.copy(hint="留空采用设备默认地址") else it })
    val accountEdit = account.copy(title="修改已有账号",op="account.patch",description="按标签、序号或完整学号匹配唯一账号；仅修改非空项，留空保留。修改学号需重新授权。",inputs=listOf(Input("_target","目标标签 / 序号 / 学号"))+account.inputs.map { it.copy(required=false,hint="留空保留设备原值") })
    val name = CommandForm("修改设备名称", "name.set", "显示为 WANAN-标签。修改后重新扫描，设备 MAC 不变。", listOf(Input("name", "名称标签", "1–12 位字母、数字、- 或 _")))
    val notify = CommandForm("配置默认通知", "notify.set", "通知地址可能包含密钥，仅发送给所选设备。Bark 使用 0，ntfy 使用 1，Server酱使用 2。", listOf(
        Input("url", "推送 URL", "https://…", kind = Kind.Secret), Input("style", "发送格式", initial = "2", kind = Kind.Choice, choices = listOf("0", "1", "2")),
        Input("gap", "去重间隔（秒）", initial = "60", kind = Kind.Int), Input("quota", "每日条数上限", "0 表示不限", "40", Kind.Int)), confirm = true)
    val notifyAdd = CommandForm("添加通知通道", "notify.add", "最多 5 条附加通道。关联账号填标签，以英文逗号分隔；all 接收全部事件。", listOf(
        Input("label", "通道标签", "1–12 位字母、数字、- 或 _"), Input("url", "推送 URL", "https://…", kind = Kind.Secret),
        Input("style", "格式", initial = "2", kind = Kind.Choice, choices = listOf("0", "1", "2")), Input("accounts", "关联账号", initial = "all")), confirm = true)
    val gap = CommandForm("多人任务间隔", "multi.set", "每个账号之间等待的秒数，范围 0–600。", listOf(Input("gap_s", "间隔（秒）", initial = "20", kind = Kind.Int)))
    val chain = CommandForm("全链路检查", "test.chain", "此操作会登录平台，不提交打卡。默认遵守设备的时间窗口策略。最长等待 900 秒。", listOf(Input("wait_s", "等待平台放行（秒）", initial = "0", kind = Kind.Int)), confirm = true)
    val wxTest = CommandForm("微信取票测试", "wx.test", "测试当前槽位的微信授权与取票链路，不提交打卡。", listOf(Input("slot", "槽位", initial = "0", kind = Kind.Choice, choices = (0..5).map(Int::toString))), confirm = true)
    val reset = CommandForm("恢复出厂设置", "factory.reset", "将擦除设备全部配置与凭据。此操作不可撤销。请输入 YES 确认。", listOf(Input("confirm", "输入 YES")), confirm = true)
    fun payload(form: CommandForm, values: Map<String, String>): JsonObject {
        val fields = buildJsonObject {
            for (input in form.inputs) {
                val value = values[input.key].orEmpty()
                require(!input.required || value.isNotBlank()) { "请填写${input.title}" }
                if (!input.required && value.isEmpty() && (input.key != "pass" || form.op=="account.patch")) continue
                if (form.op in setOf("account.add","account.patch") && input.key in setOf("lat", "lng") && value.isBlank()) continue
                when (input.kind) {
                    Kind.Int -> put(input.key, value.toLongOrNull() ?: error("${input.title}需要整数"))
                    Kind.Decimal -> put(input.key, value.toDoubleOrNull()?.takeIf { it.isFinite() } ?: error("${input.title}需要有效数字"))
                    Kind.Choice -> {
                        require(value in input.choices) { "请选择${input.title}" }
                        put(input.key, value.toIntOrNull() ?: error("选项无效"))
                    }
                    else -> put(input.key, value)
                }
            }
            if (form.op in setOf("notify.set", "notify.add")) put("on", true)
            if (form.op == "notify.enable") put("on", values["on"] == "1")
        }
        fun number(key: String) = fields[key]?.jsonPrimitive?.doubleOrNull ?: 0.0
        if (form.op in setOf("account.add","account.patch")) require(("lat" in fields) == ("lng" in fields)) { "账号经纬度需要成对填写，或同时留空" }
        if (form.op == "loc.set" || (form.op in setOf("account.add","account.patch") && "lat" in fields)) {
            require(number("lat") in -90.0..90.0 && number("lng") in -180.0..180.0) { "纬度需在 -90～90，经度需在 -180～180" }
            require(number("lat") != 0.0 && number("lng") != 0.0) { "当前固件将零坐标视为未配置，请核对宿舍坐标" }
        }
        for (key in listOf("label", "name")) if (key in fields) require(Regex("[A-Za-z0-9_-]{1,12}").matches(fields.getValue(key).jsonPrimitive.content)) { "标签仅支持 1–12 位字母、数字、- 和 _" }
        values["bfp"]?.takeIf { it.isNotEmpty() }?.let { require(Regex("[A-Fa-f0-9]{32}").matches(it)) { "浏览器指纹应为 32 位十六进制" } }
        if ("slot" in fields) require(number("slot") in 0.0..5.0) { "槽位应为 0–5" }
        if ("expiresAt" in fields) require(number("expiresAt") >= 0) { "到期时间不能为负数" }
        if (form.op == "sched.set") {
            fun minute(key: String): Int {
                val value = values[key].orEmpty()
                require(Regex("([01][0-9]|2[0-3]):[0-5][0-9]").matches(value)) { "时间格式应为 HH:MM" }
                return value.take(2).toInt() * 60 + value.takeLast(2).toInt()
            }
            val start = minute("at"); val end = minute("window_end")
            require(start in 1260..1410 && end in start..1410) { "计划应在 21:00–23:30 内，截止时间不能早于计划时间" }
            require(number("jitter_s") in 0.0..300.0) { "抖动范围为 0–300 秒" }
        }
        if ("gap_s" in fields) require(number("gap_s") in 0.0..600.0) { "间隔范围为 0–600 秒" }
        if ("wait_s" in fields) require(number("wait_s") in 0.0..900.0) { "等待范围为 0–900 秒" }
        if ("gap" in fields) require(number("gap") in 0.0..3600.0) { "去重间隔范围为 0–3600 秒" }
        if ("quota" in fields) require(number("quota") in 0.0..200.0) { "每日上限范围为 0–200" }
        if ("url" in fields) require(values["url"].orEmpty().startsWith("https://") || values["url"].orEmpty().startsWith("http://")) { "请输入 http:// 或 https:// 地址" }
        if (form.op == "factory.reset") require(values["confirm"] == "YES") { "请输入大写 YES 确认" }
        if ("ssid" in fields) require(values["ssid"].orEmpty().toByteArray().size <= 32) { "WiFi 名称不能超过 32 字节" }
        return fields
    }
}
