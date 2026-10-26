@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class)
package io.mooncheck.app.ui

import android.app.Activity
import android.view.WindowManager
import androidx.activity.compose.LocalActivity
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.rounded.*
import androidx.compose.material.icons.rounded.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.*
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.mooncheck.app.ble.LinkState
import io.mooncheck.app.data.DeviceSnapshot
import io.mooncheck.app.protocol.ProtocolCodec
import kotlinx.serialization.json.*

private data class ConfirmAction(val title: String, val text: String, val run: () -> Unit)
private val pageTitles = listOf("控制台", "配置", "账号", "帮助")

@Composable fun MoonApp(vm: ControlViewModel, state: DeviceSnapshot, link: LinkState, scan: () -> Unit) {
    var tab by rememberSaveable { mutableIntStateOf(0) }
    var devices by rememberSaveable { mutableStateOf(false) }
    var form by remember { mutableStateOf<CommandForm?>(null) }
    var confirm by remember { mutableStateOf<ConfirmAction?>(null) }
    var runtimePage by rememberSaveable { mutableStateOf(false) }
    var developer by remember { mutableStateOf(false) }
    var diagnostics by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(state.ready) {
        if (!state.ready) { form = null; confirm = null; developer = false }
    }
    val enabled = state.ready && !state.busy
    fun execute(title: String, op: String, fields: JsonObject = JsonObject(emptyMap()), timeout: Long = 15000) { vm.repository.execute(title, op, fields, timeout) }
    val pick: (CommandForm) -> Unit = { form = it }
    Scaffold(containerColor = MaterialTheme.colorScheme.background,
        topBar = {
            CenterAlignedTopAppBar(title = { Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Rounded.NightsStay, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(23.dp))
                Spacer(Modifier.width(8.dp)); Text("MoonCheck", fontWeight = FontWeight.SemiBold, fontSize = 19.sp)
            } }, actions = { IconButton(onClick = { devices = true }) { Icon(Icons.Rounded.Bluetooth, "连接与设备") } },
                colors = TopAppBarDefaults.centerAlignedTopAppBarColors(containerColor = MaterialTheme.colorScheme.background))
        }, bottomBar = {
            NavigationBar(containerColor = MaterialTheme.colorScheme.surface, tonalElevation = 0.dp) {
                val icons = listOf(Icons.Rounded.SpaceDashboard, Icons.Rounded.Tune, Icons.Rounded.PeopleAlt, Icons.AutoMirrored.Rounded.HelpOutline)
                pageTitles.forEachIndexed { index, title -> NavigationBarItem(selected = tab == index,
                    onClick = { tab = index; diagnostics = false; runtimePage = false; developer = false; vm.repository.clearLogs() }, icon = { Icon(icons[index], title) }, label = { Text(title) }) }
            }
        }
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.TopCenter) {
            Column(Modifier.widthIn(max = 840.dp).fillMaxSize().verticalScroll(rememberScrollState()).padding(horizontal = 22.dp).padding(bottom = 28.dp), verticalArrangement = Arrangement.spacedBy(18.dp)) {
                if (tab != 3) {
                    Surface(shape = RoundedCornerShape(16.dp), color = MaterialTheme.colorScheme.surfaceVariant, onClick = { devices = true }) {
                        Row(Modifier.fillMaxWidth().padding(14.dp), verticalAlignment = Alignment.CenterVertically) {
                            Icon(if (state.ready) Icons.Rounded.BluetoothConnected else Icons.AutoMirrored.Rounded.BluetoothSearching, null, tint = MaterialTheme.colorScheme.primary)
                            Column(Modifier.padding(start = 12.dp).weight(1f)) {
                                Text(if (state.ready) link.device?.name ?: "MoonCheck 设备" else "连接你的设备", style = MaterialTheme.typography.titleSmall)
                                Text(link.phase, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                            }
                            Icon(Icons.Rounded.ChevronRight, null)
                        }
                    }
                    if (state.busy) LinearProgressIndicator(Modifier.fillMaxWidth(), color = MaterialTheme.colorScheme.primary)
                }
                when (tab) {
                    0 -> if (runtimePage) {
                        TextButton(onClick = { runtimePage = false; developer = false; vm.repository.clearLogs() }) { Icon(Icons.AutoMirrored.Rounded.ArrowBack, null); Text("返回控制台") }
                        RuntimePanel(state, enabled, developer,
                            refresh = { execute("刷新运行状态", "status") },
                            toggleDeveloper = { developer = it; if (!it) vm.repository.clearLogs() },
                            readLogs = { execute("读取设备日志", "logs", buildJsonObject { put("n", 30) }) },
                            clearLogs = { vm.repository.clearLogs() })
                    } else if (diagnostics) {
                        TextButton(onClick = { diagnostics = false }) { Icon(Icons.AutoMirrored.Rounded.ArrowBack, null); Text("返回控制台") }
                        PageHeading("诊断工具", "逐项检查，明确问题发生在哪里")
                        Section("基础检查") {
                            ActionRow("查看设备时间", "检查校时与当前窗口", Icons.Rounded.Schedule, enabled) { execute("时间检查", "test.time") }
                            ActionRow("网络连通检查", "由设备访问 generate_204", Icons.Rounded.NetworkCheck, enabled) { execute("网络检查", "test.http", buildJsonObject { put("url", "http://connect.rom.miui.com/generate_204") }, 120000) }
                            ActionRow("微信取票测试", "只测试，不提交打卡", Icons.Rounded.VerifiedUser, enabled) { pick(Forms.wxTest) }
                            ActionRow("全链路检查", "可能登录平台，不提交打卡", Icons.Rounded.Route, enabled) { pick(Forms.chain) }
                            ActionRow("开发者日志", "查看脱敏日志与内存状态", Icons.AutoMirrored.Rounded.Article, true) { runtimePage = true; developer = false }
                        }
                        Section("最近活动") {
                            if (state.events.isEmpty()) Text("暂无活动。完成操作后将在这里显示。", style = MaterialTheme.typography.bodyMedium)
                            state.events.forEach { Text("•  $it", style = MaterialTheme.typography.bodyMedium) }
                        }
                    } else {
                        PageHeading("每晚，从容一点。", "让设备照常运行，让控制更加简单")
                        Hero(state, enabled,
                            refresh = { execute("刷新状态", "status") },
                            connect = { devices = true })
                        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                            Metric("网络", if (!state.ready) "待连接" else if (state.wifiConnected) "已联网" else "未联网", Icons.Rounded.Wifi, Modifier.weight(1f))
                            Metric("设备时间", if (!state.ready) "待同步" else if (state.synced) "已校时" else "未校时", Icons.Rounded.Schedule, Modifier.weight(1f))
                        }
                        Section("快捷操作") {
                            ActionRow("运行状态", "网络名称、内存峰值与开发者日志", Icons.Rounded.Memory, true) { runtimePage = true }
                            ActionRow("设备体检", "检查当前会话和前置状态", Icons.Rounded.HealthAndSafety, enabled) { execute("设备体检", "test.probe", timeout = 120000) }
                            ActionRow("手动提交打卡", "会真实提交，执行前请确认", Icons.Rounded.TaskAlt, enabled) {
                                confirm = ConfirmAction("提交本次打卡？", "将向当前设备发送真实提交指令。默认遵守时间窗口且不跳过今日已提交检查。请确认设备与账号正确。") {
                                    execute("手动打卡", "test.checkin", buildJsonObject { put("force", false) }, 120000)
                                }
                            }
                            ActionRow("诊断与最近活动", "网络、取票、链路检查", Icons.AutoMirrored.Rounded.ManageSearch, true) { diagnostics = true }
                        }
                        Section("最近结果") {
                            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                Icon(if (state.lastOk == true) Icons.Rounded.CheckCircle else Icons.Rounded.CalendarToday, null, tint = MaterialTheme.colorScheme.primary)
                                Column {
                                    Text(when { !state.ready -> "连接后查看设备记录"; state.lastOk == null -> "设备暂无结果"; state.lastOk -> "最近记录成功"; else -> "最近记录未成功" }, style = MaterialTheme.typography.titleSmall)
                                    Text(if (state.ready) state.lastDate else "历史记录以设备回读为准", style = MaterialTheme.typography.bodySmall)
                                }
                            }
                        }
                        Note("第一次使用？", "在配置页完成 5 个步骤，再验证授权并开启自动任务。", Icons.Rounded.AutoAwesome) { tab = 1 }
                    }
                    1 -> {
                        PageHeading("一次配置，安心运行", "每一步独立保存，以设备回执确认")
                        val steps = listOf(Forms.wifi, Forms.auth, Forms.wx, Forms.location, Forms.schedule)
                        val descriptions = listOf("让设备接入宿舍的 2.4 GHz 网络", "学号、密码与浏览器指纹", "本人授权凭据与对应槽位", "现场确认的纬度、经度和地址", "设置时间、截止窗口与随机抖动")
                        Section("配置向导 · 本次连接已完成 ${state.configured.size.coerceAtMost(5)}/5") {
                            steps.forEachIndexed { index, step ->
                                ActionRow(step.title, descriptions[index], if (step.step in state.configured) Icons.Rounded.CheckCircle else listOf(Icons.Rounded.Wifi, Icons.Rounded.PersonOutline, Icons.Rounded.VerifiedUser, Icons.Rounded.LocationOn, Icons.Rounded.Schedule)[index], enabled, badge = "0${index + 1}") { pick(step) }
                            }
                        }
                        Section("每日自动任务") {
                            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                                Column(Modifier.weight(1f)) { Text("自动运行", style = MaterialTheme.typography.titleMedium); Text("设备独立执行，无需手机常驻", style = MaterialTheme.typography.bodySmall) }
                                Switch(checked = state.ready && state.auto, enabled = enabled, onCheckedChange = { on ->
                                    confirm = ConfirmAction(if (on) "开启每日自动任务？" else "暂停每日自动任务？", if (on) "请确认 WiFi、本人账号、微信授权、宿舍坐标和计划已在设备保存，且体检与取票测试通过。已有配置可直接确认，无需每次重新填写。" else "暂停后设备不会执行每日自动打卡；保存的配置会保留。") {
                                        execute(if (on) "开启自动任务" else "暂停自动任务", "auto.set", buildJsonObject { put("enabled", on) })
                                    }
                                })
                            }
                            Text("计划 ${if (state.ready) state.at else "—"}  ·  截止 ${if (state.ready) state.end else "—"}", color = MaterialTheme.colorScheme.primary, style = MaterialTheme.typography.bodyMedium)
                        }
                        Section("通知与设备") {
                            ActionRow("默认通知渠道", "Bark、ntfy、Server酱", Icons.Rounded.NotificationsNone, enabled) { pick(Forms.notify) }
                            ActionRow("附加通知通道", "按账号标签关联推送", Icons.Rounded.AddAlert, enabled) { pick(Forms.notifyAdd) }
                            ActionRow("修改设备名称", "为多块板子设置易认的标签", Icons.Rounded.Edit, enabled) { pick(Forms.name) }
                            ActionRow("更多高级操作", "通知测试、通道管理与恢复出厂", Icons.Rounded.SettingsSuggest, enabled) { pick(CommandForm("管理操作", "menu", "", emptyList())) }
                        }
                        if (state.ready) Section("通知通道状态") {
                            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                                Text("默认通知", Modifier.weight(1f), style = MaterialTheme.typography.titleSmall)
                                Switch(checked = state.notifyEnabled, enabled = enabled, onCheckedChange = { on ->
                                    execute(if (on) "启用默认通知" else "暂停默认通知", "notify.set", buildJsonObject { put("on", on) })
                                })
                            }
                            if (state.channels.isEmpty()) Text("暂无附加通知通道", style = MaterialTheme.typography.bodySmall)
                            state.channels.forEach { channel ->
                                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                                    Column(Modifier.weight(1f)) { Text(channel.label, style = MaterialTheme.typography.titleSmall); Text("关联 ${channel.accounts}", style = MaterialTheme.typography.bodySmall) }
                                    Switch(checked = channel.enabled, enabled = enabled, onCheckedChange = { on -> execute("更新通道开关", "notify.enable", buildJsonObject { put("label", channel.label); put("on", on) }) })
                                }
                            }
                        }
                        Note("配置提示", "输入只用于发送给当前设备。关闭表单会清空未提交内容；离开应用将断开连接。", Icons.Rounded.PrivacyTip)
                    }
                    2 -> {
                        PageHeading("账号与授权", "每人独立授权，按槽位清晰管理")
                        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                            Button(onClick = { pick(Forms.account) }, enabled = enabled) { Icon(Icons.Rounded.Add, null, Modifier.size(18.dp)); Text(" 添加账号") }
                            OutlinedButton(onClick = { execute("刷新账号与授权", "account.list") }, enabled = enabled) { Text("刷新列表") }
                        }
                        if (state.accounts.isEmpty()) Section("${if (state.ready) "单人配置模式" else "连接后读取账号"}") {
                            Icon(Icons.Rounded.PersonOutline, null, Modifier.size(34.dp), tint = MaterialTheme.colorScheme.primary)
                            Text("账号表为空时，设备使用配置页的本人账号与槽位 0。", style = MaterialTheme.typography.bodyMedium)
                            Text(if (0 in state.slots) "槽位 0 · 已保存授权" else "槽位 0 · 尚未读取到授权", color = MaterialTheme.colorScheme.primary)
                            OutlinedButton(onClick = { pick(Forms.wx) }, enabled = enabled) { Text("导入本人授权") }
                        }
                        state.accounts.forEach { account ->
                            Section("${account.label}  ·  槽位 ${account.slot}") {
                                Text(account.user, style = MaterialTheme.typography.bodyMedium)
                                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                                    Text(if (account.authorized) "已保存微信授权" else "未保存微信授权", Modifier.weight(1f), color = MaterialTheme.colorScheme.primary)
                                    Switch(checked = account.enabled, enabled = enabled, onCheckedChange = { on ->
                                        execute(if (on) "启用账号" else "暂停账号", "account.enable", buildJsonObject { put("label", account.label); put("on", on) })
                                    })
                                }
                                Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                                    OutlinedButton(onClick = { pick(Forms.wx.copy(inputs = Forms.wx.inputs.map { if (it.key == "slot") it.copy(initial = account.slot.toString()) else it })) }, enabled = enabled) { Text("导入授权") }
                                    TextButton(onClick = { confirm = ConfirmAction("删除 ${account.label}？", "删除后必须重新核对所有账号的微信槽位，应用不会自动移动凭据。") { execute("删除账号", "account.remove", buildJsonObject { put("label", account.label) }) } }, enabled = enabled) { Text("删除", color = MaterialTheme.colorScheme.error) }
                                }
                            }
                        }
                        Section("多人运行") {
                            ActionRow("设置账号间隔", "默认 20 秒，最长 600 秒", Icons.Rounded.Timelapse, enabled) { pick(Forms.gap) }
                            ActionRow("多人演练", "会逐个登录，不提交打卡", Icons.Rounded.Groups, enabled) {
                                confirm = ConfirmAction("开始多人演练？", "将逐个执行登录和前置检查，不提交打卡。任务可能持续数分钟，默认遵守设备时间窗口。") { execute("多人演练", "test.multi", buildJsonObject { put("probe", true); put("force", false) }, 1800000) }
                            }
                        }
                        Note("先核对，再导入", "账号增删或覆盖后，重新确认标签与槽位。授权状态表示已保存，不等于当前一定有效。", Icons.Rounded.Info)
                    }
                    3 -> HelpPage()
                }
                if (tab != 3) Surface(shape = RoundedCornerShape(16.dp), color = if (state.error) MaterialTheme.colorScheme.errorContainer else MaterialTheme.colorScheme.surfaceVariant) {
                    Row(Modifier.padding(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        Icon(if (state.error) Icons.Rounded.Info else Icons.Rounded.ChatBubbleOutline, null, Modifier.size(20.dp))
                        Text(state.message, style = MaterialTheme.typography.bodyMedium)
                    }
                }
            }
        }
    }
    if (devices) DeviceSheet(vm, link, state, scan) { devices = false }
    if (form != null) {
        if (form!!.op == "menu") AdvancedSheet(enabled, pick, { title, op, fields ->
            confirm = ConfirmAction("$title？", "此操作会发送到当前选定设备，请确认后继续。") { execute(title, op, fields, 120000) }
        }, { form = null })
        else CommandSheet(form!!, link, state, { selected, values ->
            val payload = Forms.payload(selected, values)
            val timeout = when (selected.op) { "wifi.set", "wx.test" -> 120000L; "test.chain" -> (values["wait_s"]!!.toLong() + 120) * 1000; else -> 15000L }
            val send = { vm.repository.execute(selected.title, selected.op, payload, timeout, selected.step) }
            if (selected.confirm) confirm = ConfirmAction("确认${selected.title}？", "将发送到 ${link.device?.name ?: "当前设备"}。${selected.description}", send) else send()
            form = null
        }, { form = null })
    }
    confirm?.let { action -> AlertDialog(onDismissRequest = { confirm = null }, icon = { Icon(Icons.Rounded.Info, null) }, title = { Text(action.title) }, text = { Text(action.text) },
        confirmButton = { TextButton(onClick = { confirm = null; action.run() }, enabled = !state.busy) { Text("确认执行") } }, dismissButton = { TextButton(onClick = { confirm = null }) { Text("取消") } }) }
}

@Composable private fun PageHeading(title: String, subtitle: String) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(title, style = MaterialTheme.typography.headlineSmall)
        Text(subtitle, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
@Composable private fun Hero(state: DeviceSnapshot, enabled: Boolean, refresh: () -> Unit, connect: () -> Unit) {
    Box(Modifier.fillMaxWidth().background(Brush.linearGradient(listOf(Color(0xFF254B3A), Color(0xFF4E7060))), RoundedCornerShape(28.dp)).padding(24.dp)) {
        Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                Text("YOUR QUIET COMPANION", color = Color(0xFFCFDFCD), fontSize = 10.sp, letterSpacing = 1.5.sp)
                Icon(Icons.Rounded.NightsStay, null, tint = Color(0xFFE6D6A7), modifier = Modifier.size(32.dp))
            }
            Text(if (!state.ready) "晚安，从这里开始" else if (state.auto) "自动计划已开启" else "设备已就绪", color = Color.White, style = MaterialTheme.typography.headlineSmall)
            Text(if (!state.ready) "连接宿舍设备，轻松完成配置与日常控制。" else "${state.wifi}  ·  ${state.at} 计划\n${state.time}", color = Color(0xFFDEEBDB), style = MaterialTheme.typography.bodyMedium)
            FilledTonalButton(onClick = if (state.ready) refresh else connect, enabled = if (state.ready) enabled else !state.busy,
                colors = ButtonDefaults.filledTonalButtonColors(containerColor = Color(0xFFE9EEDC), contentColor = Color(0xFF254B3A))) {
                Icon(if (state.ready) Icons.Rounded.Refresh else Icons.Rounded.Bluetooth, null, Modifier.size(18.dp)); Spacer(Modifier.width(8.dp)); Text(if (state.ready) "刷新设备状态" else "连接设备")
            }
        }
    }
}
@Composable private fun Section(title: String, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth().background(MaterialTheme.colorScheme.surface, RoundedCornerShape(22.dp)).padding(18.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
        content()
    }
}
@Composable private fun Metric(title: String, value: String, icon: ImageVector, modifier: Modifier) {
    Column(modifier.background(MaterialTheme.colorScheme.surface, RoundedCornerShape(20.dp)).padding(18.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Icon(icon, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(22.dp))
        Text(value, style = MaterialTheme.typography.titleLarge)
        Text(title, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
@Composable private fun ActionRow(title: String, description: String, icon: ImageVector, enabled: Boolean, badge: String? = null, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth().clipShape().clickable(enabled = enabled, onClick = onClick).padding(vertical = 10.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Surface(shape = RoundedCornerShape(13.dp), color = MaterialTheme.colorScheme.primaryContainer.copy(alpha = if (enabled) 1f else .45f)) { Box(Modifier.size(44.dp), contentAlignment = Alignment.Center) { Icon(icon, null, tint = MaterialTheme.colorScheme.primary, modifier = Modifier.size(22.dp)) } }
        Column(Modifier.weight(1f)) { Text(title, style = MaterialTheme.typography.titleSmall, color = if (enabled) MaterialTheme.colorScheme.onSurface else MaterialTheme.colorScheme.onSurfaceVariant); Text(description, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant) }
        if (badge != null) Text(badge, style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.onSurfaceVariant) else Icon(Icons.Rounded.ChevronRight, null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
private fun Modifier.clipShape() = this.then(Modifier.background(Color.Transparent, RoundedCornerShape(12.dp)))
@Composable private fun Note(title: String, text: String, icon: ImageVector, click: (() -> Unit)? = null) {
    Surface(shape = RoundedCornerShape(20.dp), color = MaterialTheme.colorScheme.secondaryContainer, modifier = if (click != null) Modifier.clickable(onClick = click) else Modifier) {
        Row(Modifier.padding(18.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Icon(icon, null, Modifier.size(22.dp)); Column { Text(title, style = MaterialTheme.typography.titleSmall); Spacer(Modifier.height(4.dp)); Text(text, style = MaterialTheme.typography.bodyMedium) }
        }
    }
}

@Composable private fun DeviceSheet(vm: ControlViewModel, link: LinkState, state: DeviceSnapshot, scan: () -> Unit, dismiss: () -> Unit) {
    ModalBottomSheet(onDismissRequest = dismiss, sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {
        Column(Modifier.fillMaxWidth().heightIn(max = 680.dp).verticalScroll(rememberScrollState()).padding(horizontal = 24.dp).padding(bottom = 32.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            PageHeading("附近设备", "核对名称与 MAC，选择你自己的板子")
            Text(link.phase, color = MaterialTheme.colorScheme.primary)
            if (state.error) Text(state.message, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodyMedium)
            if (link.connected) Section(link.device?.name ?: "当前设备") {
                Text("固件 ${state.firmware}  ·  MTU ${link.mtu}")
                Text("蓝牙 MAC  ${state.mac}", style = MaterialTheme.typography.bodySmall)
                OutlinedButton(onClick = { vm.repository.disconnect() }) { Text("断开连接") }
            }
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                Button(onClick = scan, enabled = !state.busy && !link.scanning && !link.connected) { Icon(Icons.Rounded.Search, null); Text(" 扫描设备") }
                if (link.scanning) TextButton(onClick = { vm.transport.stopScan() }) { Text("停止扫描") }
            }
            if (link.scanning) LinearProgressIndicator(Modifier.fillMaxWidth())
            if (state.busy && !state.ready) TextButton(onClick = { vm.repository.disconnect() }) { Text("取消连接") }
            if (link.devices.isEmpty()) Note("${if (link.scanning) "正在寻找…" else "还没有可选设备"}", "给 ESP32 上电，靠近手机并开始扫描。请先关闭占用设备的其它蓝牙工具。", Icons.AutoMirrored.Rounded.BluetoothSearching)
            link.devices.forEach { device ->
                OutlinedCard(onClick = { vm.repository.connect(device) }, enabled = !state.busy && !link.connected, shape = RoundedCornerShape(18.dp)) {
                    Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Rounded.DeveloperBoard, null, tint = MaterialTheme.colorScheme.primary)
                        Column(Modifier.weight(1f).padding(horizontal = 14.dp)) { Text(device.name, fontWeight = FontWeight.SemiBold); Text(device.address, style = MaterialTheme.typography.bodySmall) }
                        Text("${device.rssi}\ndBm", style = MaterialTheme.typography.labelSmall)
                    }
                }
            }
            Text("多台设备不会自动选择。若系统权限被拒绝，请在系统设置中开启“附近设备”权限。", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable private fun CommandSheet(form: CommandForm, link: LinkState, state: DeviceSnapshot, submit: (CommandForm, Map<String, String>) -> Unit, dismiss: () -> Unit) {
    val values = remember(form) { mutableStateMapOf<String, String>().apply { form.inputs.forEach { put(it.key, it.initial) } } }
    var error by remember(form) { mutableStateOf<String?>(null) }
    val activity = LocalActivity.current
    DisposableEffect(Unit) { activity?.window?.addFlags(WindowManager.LayoutParams.FLAG_SECURE); onDispose { values.clear(); if (!link.connected) activity?.window?.clearFlags(WindowManager.LayoutParams.FLAG_SECURE) } }
    ModalBottomSheet(onDismissRequest = { values.clear(); dismiss() }, sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {
        Column(Modifier.fillMaxWidth().imePadding().verticalScroll(rememberScrollState()).padding(horizontal = 24.dp).padding(bottom = 30.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
            PageHeading(form.title, form.description)
            Text("发送至 ${link.device?.name ?: "未连接"}\n${link.device?.address ?: ""}", style = MaterialTheme.typography.labelMedium, color = MaterialTheme.colorScheme.primary)
            form.inputs.forEach { input ->
                var visible by remember(input.key) { mutableStateOf(false) }
                if (input.kind == Kind.Choice) {
                    Text(input.title, style = MaterialTheme.typography.titleSmall)
                    Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        input.choices.forEach { choice -> FilterChip(selected = values[input.key] == choice, onClick = { values[input.key] = choice }, label = { Text(choice) }) }
                    }
                    if (input.hint.isNotEmpty()) Text(input.hint, style = MaterialTheme.typography.bodySmall)
                } else {
                    OutlinedTextField(value = values[input.key].orEmpty(), onValueChange = { values[input.key] = it; error = null }, modifier = Modifier.fillMaxWidth(),
                        label = { Text(input.title + if (input.required) "" else "（可选）") }, supportingText = if (input.hint.isNotEmpty()) ({ Text(input.hint) }) else null,
                        singleLine = true, shape = RoundedCornerShape(14.dp),
                        visualTransformation = if (input.kind == Kind.Secret && !visible) PasswordVisualTransformation() else VisualTransformation.None,
                        trailingIcon = if (input.kind == Kind.Secret) ({ IconButton(onClick = { visible = !visible }) { Icon(if (visible) Icons.Rounded.VisibilityOff else Icons.Rounded.Visibility, if (visible) "隐藏内容" else "显示内容") } }) else null,
                        keyboardOptions = KeyboardOptions(autoCorrectEnabled = false, keyboardType = when (input.kind) { Kind.Int -> KeyboardType.Number; Kind.Decimal -> KeyboardType.Decimal; Kind.Secret -> KeyboardType.Password; else -> KeyboardType.Text }))
                    if (input.key == "ssid" && state.networks.isNotEmpty()) Row(Modifier.horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        state.networks.forEach { ssid -> SuggestionChip(onClick = { values["ssid"] = ssid }, label = { Text(ssid) }) }
                    }
                }
            }
            if (form.op == "wifi.set") Text("可先关闭此表单，到配置页的高级操作中扫描 WiFi。", style = MaterialTheme.typography.bodySmall)
            error?.let { Text(it, color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodyMedium) }
            Button(onClick = {
                try {
                    val payload = Forms.payload(form, values)
                    require(ProtocolCodec.request(Int.MAX_VALUE - 1, form.op, payload).size - 1 <= state.maxCommandBytes) { "当前固件无法接收这么长的配置，请先升级支持长命令的固件。" }
                    submit(form, values.toMap()); values.clear()
                }
                catch (e: IllegalArgumentException) { error = e.message ?: "请检查输入" }
                catch (e: IllegalStateException) { error = e.message ?: "请检查输入" }
            }, enabled = state.ready && !state.busy, modifier = Modifier.fillMaxWidth().heightIn(min = 52.dp), shape = RoundedCornerShape(16.dp)) { Text(if (form.confirm) "核对并继续" else "保存到设备") }
            TextButton(onClick = { values.clear(); dismiss() }, modifier = Modifier.align(Alignment.CenterHorizontally)) { Text("取消并清空输入") }
        }
    }
}

@Composable private fun AdvancedSheet(enabled: Boolean, pick: (CommandForm) -> Unit, run: (String, String, JsonObject) -> Unit, dismiss: () -> Unit) {
    ModalBottomSheet(onDismissRequest = dismiss, sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(horizontal = 24.dp).padding(bottom = 32.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            PageHeading("高级操作", "只在了解作用后执行")
            ActionRow("扫描 WiFi", "由设备扫描 2.4 GHz 网络", Icons.Rounded.WifiFind, enabled) { dismiss(); run("扫描 WiFi", "wifi.scan", JsonObject(emptyMap())) }
            ActionRow("测试默认通知", "会真实发送一条测试消息", Icons.Rounded.NotificationsActive, enabled) { dismiss(); run("测试通知", "test.notify", buildJsonObject { put("body", "MoonCheck 测试通知") }) }
            ActionRow("通知通道开关", "按标签启用或暂停附加通道", Icons.Rounded.ToggleOn, enabled) { pick(CommandForm("通知通道开关", "notify.enable", "输入已存在的附加通道标签。", listOf(Input("label", "通道标签"), Input("on", "是否启用", initial = "1", kind = Kind.Choice, choices = listOf("1", "0"))), confirm = true)) }
            ActionRow("删除通知通道", "删除一个附加通道", Icons.Rounded.DeleteOutline, enabled) { pick(CommandForm("删除通知通道", "notify.remove", "删除后该通道不再接收推送。", listOf(Input("label", "通道标签")), confirm = true)) }
            ActionRow("重设通道关联", "更新推送关联的账号标签", Icons.Rounded.Link, enabled) { pick(CommandForm("重设通道关联", "notify.link", "all 表示全部事件；多个账号标签用英文逗号分隔。", listOf(Input("label", "通道标签"), Input("accounts", "关联账号", initial = "all")), confirm = true)) }
            ActionRow("清空账号表", "回到单人配置；需重新核对授权槽位", Icons.Rounded.PersonRemove, enabled) { dismiss(); run("清空账号表", "account.clear", JsonObject(emptyMap())) }
            ActionRow("恢复出厂设置", "擦除全部配置与凭据", Icons.Rounded.RestartAlt, enabled) { pick(Forms.reset) }
        }
    }
}

@Composable private fun HelpPage() {
    var query by rememberSaveable { mutableStateOf("") }
    var open by rememberSaveable { mutableStateOf<String?>("第一次使用：从连接到自动运行") }
    val uri = LocalUriHandler.current
    PageHeading("随时有帮助", "从第一次连接，到日常使用与排错")
    OutlinedTextField(query, { query = it }, Modifier.fillMaxWidth(), placeholder = { Text("搜索 WiFi、授权、连接…") }, leadingIcon = { Icon(Icons.Rounded.Search, null) }, singleLine = true, shape = RoundedCornerShape(18.dp))
    val articles = helpArticles.filter { query.isBlank() || it.title.contains(query, true) || it.body.contains(query, true) || it.category.contains(query, true) }
    if (articles.isEmpty()) Note("没有找到相关说明", "试试“连接”“微信”或“时间”等关键词。", Icons.Rounded.SearchOff)
    articles.forEach { article ->
        Surface(shape = RoundedCornerShape(20.dp), color = MaterialTheme.colorScheme.surface) {
            Column(Modifier.fillMaxWidth()) {
                Row(Modifier.fillMaxWidth().clickable { open = if (open == article.title) null else article.title }.padding(18.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) { Text(article.category, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.primary); Spacer(Modifier.height(5.dp)); Text(article.title, style = MaterialTheme.typography.titleSmall) }
                    Icon(if (open == article.title) Icons.Rounded.ExpandLess else Icons.Rounded.ExpandMore, "展开或收起")
                }
                AnimatedVisibility(open == article.title) { Text(article.body, Modifier.padding(horizontal = 18.dp).padding(bottom = 20.dp), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant) }
            }
        }
    }
    OutlinedButton(onClick = { uri.openUri("https://github.com/richard297779/UESTC-MoonCheck") }, modifier = Modifier.fillMaxWidth()) { Icon(Icons.AutoMirrored.Rounded.OpenInNew, null, Modifier.size(18.dp)); Text(" 查看项目完整指南") }
    Text("帮助内容可离线阅读；外部指南将通过浏览器打开。", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
}


@Composable private fun RuntimePanel(state: DeviceSnapshot, enabled: Boolean, developer: Boolean,
    refresh: () -> Unit, toggleDeveloper: (Boolean) -> Unit, readLogs: () -> Unit, clearLogs: () -> Unit) {
    val runtime = state.runtime
    fun bytes(value: Long?): String = value?.let { String.format(java.util.Locale.ROOT, "%.1f KiB", it / 1024.0) } ?: "固件未提供"
    PageHeading("运行状态", "设备实时回读 · 手动刷新，降低蓝牙负担")
    OutlinedButton(onClick = refresh, enabled = enabled, modifier = Modifier.fillMaxWidth()) { Icon(Icons.Rounded.Refresh, null); Spacer(Modifier.width(8.dp)); Text("刷新运行状态") }
    Text(if (!state.ready) "当前未连接，连接后读取设备状态" else if (state.statusReadAt > 0) "最近读取：" + java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.CHINA).format(java.util.Date(state.statusReadAt)) else "尚未读取",
        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
    Section("网络与设备") {
        RuntimeValue("已连接网络", if (state.ready && state.wifiConnected) state.wifi else "未连接")
        RuntimeValue("IP 地址", runtime.ip)
        RuntimeValue("信号强度", runtime.rssi?.let { "$it dBm" } ?: "固件未提供")
        RuntimeValue("板型", runtime.board)
        RuntimeValue("芯片", runtime.chip)
        RuntimeValue("固件版本", state.firmware)
        RuntimeValue("运行时长", runtime.uptime?.let { "${it / 86400} 天 ${(it / 3600) % 24} 小时 ${(it / 60) % 60} 分" } ?: "固件未提供")
    }
    if (runtime.board != "—" && runtime.board !in setOf("s3-n16r8", "wroom32-4mb")) {
        Text("此板型未经实际验证，欢迎向开发者反馈；请查看帮助中的硬件验证说明。", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.tertiary)
    }
    Section("内部内存") {
        RuntimeValue("堆总量", bytes(runtime.total))
        RuntimeValue("当前可用", bytes(runtime.free))
        RuntimeValue("使用峰值 · 本次开机", bytes(runtime.peak))
        RuntimeValue("历史最低可用", bytes(runtime.minimum))
        RuntimeValue("最大连续可用块", bytes(runtime.largest))
        Text("峰值为分配器低水位推算的诊断值，非精确分配追踪；重启后重新统计，不含外部 PSRAM。旧固件缺失的指标显示“固件未提供”。", style = MaterialTheme.typography.bodySmall)
    }
    Section("外部 PSRAM") {
        if (runtime.psramTotal == 0L) Text("此设备没有可用 PSRAM") else {
            RuntimeValue("总量", bytes(runtime.psramTotal)); RuntimeValue("当前可用", bytes(runtime.psramFree))
            RuntimeValue("历史最低可用", bytes(runtime.psramMinimum))
        }
    }
    Section("开发者选项") {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) { Text("查看设备日志", style = MaterialTheme.typography.titleMedium); Text("仅当前连接保留，敏感行会隐藏", style = MaterialTheme.typography.bodySmall) }
            Switch(checked = developer, onCheckedChange = toggleDeveloper, enabled = state.ready)
        }
        if (developer) {
            RuntimeValue("网络任务栈最低余量", bytes(runtime.stackMinimum))
            Text("只读取设备已有日志，不触发登录、取票或提交。日志可能仍含网络名等环境信息，请勿直接公开。", style = MaterialTheme.typography.bodySmall)
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedButton(onClick = readLogs, enabled = enabled) { Text("读取最近 30 行") }
                TextButton(onClick = clearLogs) { Text("清空手机显示") }
            }
            Text("设备环形日志已覆盖 ${state.logDropped} 行", style = MaterialTheme.typography.bodySmall)
            if (state.deviceLogs.isEmpty()) Text("暂无日志，点击读取后查看。", style = MaterialTheme.typography.bodyMedium)
            state.deviceLogs.forEach { line ->
                Text(line, fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}

@Composable private fun RuntimeValue(label: String, value: String) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
        Text(label, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium, fontWeight = FontWeight.Medium)
    }
}
