package io.mooncheck.app.data

import io.mooncheck.app.ble.BleTransport
import io.mooncheck.app.ble.DeviceTransport
import io.mooncheck.app.ble.NearbyDevice
import io.mooncheck.app.protocol.*
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.sync.Mutex
import kotlinx.serialization.json.*

data class Account(val label: String, val user: String, val enabled: Boolean, val slot: Int, val authorized: Boolean)
data class NotifyChannel(val label: String, val enabled: Boolean, val accounts: String)
data class DeviceSnapshot(
    val ready: Boolean = false, val firmware: String = "—", val mac: String = "—", val maxCommandBytes: Int = 511,
    val wifi: String = "未读取", val wifiConnected: Boolean = false, val synced: Boolean = false,
    val time: String = "—", val auto: Boolean = false, val at: String = "22:00",
    val end: String = "23:30", val lastDate: String = "—", val lastOk: Boolean? = null,
    val configured: Set<String> = emptySet(), val accounts: List<Account> = emptyList(),
    val slots: Set<Int> = emptySet(), val networks: List<String> = emptyList(),
    val notifyEnabled: Boolean = false, val channels: List<NotifyChannel> = emptyList(),
    val events: List<String> = emptyList(), val busy: Boolean = false, val action: String = "",
    val message: String = "连接设备后，即可查看状态与使用快捷操作。", val error: Boolean = false
)

/** Owns one request at a time. Only approved, non-secret fields reach UI state. */
class DeviceRepository(val transport: DeviceTransport, private val scope: CoroutineScope) {
    private val mutable = MutableStateFlow(DeviceSnapshot())
    val state = mutable.asStateFlow()
    private val lock = Mutex()
    private var nextId = 1
    private var pending: Pair<Int, CompletableDeferred<JsonObject>>? = null
    private val assembler = FrameAssembler()
    private var generation = 0
    private var connecting: Job? = null
    private var expiry: Job? = null
    init {
        transport.onBytes = { bytes ->
            try { assembler.feed(bytes).forEach(::receive) }
            catch (_: Exception) { pending?.second?.completeExceptionally(IllegalStateException("设备回执不完整，请重新查询")) }
        }
        transport.onDisconnect = {
            generation++; assembler.reset()
            pending?.second?.completeExceptionally(IllegalStateException("连接已断开，操作结果未知")); pending = null
            mutable.update { it.copy(ready = false, configured = emptySet(), slots = emptySet(), accounts = emptyList(), wifiConnected = false) }
        }
        expiry = scope.launch {
            while (isActive) {
                delay(1000)
                try { assembler.expire() } catch (_: Exception) { pending?.second?.completeExceptionally(IllegalStateException("设备回执缺片，请重连后查询")) }
            }
        }
    }
    private fun receive(frame: JsonObject) {
        val id = (frame["id"] as? JsonPrimitive)?.intOrNull
        val event = frame.text("evt")
        if (id == pending?.first && event in setOf("ok", "error")) pending?.second?.complete(frame)
        else if (event in setOf("progress", "alert", "result")) {
            val stage = frame.text("stage")
            val known = mapOf("chain" to "链路检查", "probe" to "设备体检", "checkin" to "打卡任务", "wx" to "微信取票")
            val summary = when (event) {
                "progress" -> "${known[stage] ?: "设备任务"}正在处理"
                "alert" -> "设备报告提醒，请检查授权或任务状态"
                else -> "设备任务有新结果，请刷新状态确认"
            }
            mutable.update { it.copy(events = (listOf(summary) + it.events).take(20)) }
        }
    }
    private suspend fun request(op: String, fields: JsonObject = JsonObject(emptyMap()), timeout: Long = 15000): JsonObject {
        check(transport.state.value.connected) { "请先连接设备" }
        val id = nextId++
        if (nextId == Int.MAX_VALUE) nextId = 1
        val bytes = ProtocolCodec.request(id, op, fields)
        require(bytes.size - 1 <= mutable.value.maxCommandBytes) { "当前固件无法接收这么长的配置，请升级支持长命令的固件。" }
        val deferred = CompletableDeferred<JsonObject>()
        val session = generation
        pending = id to deferred
        var fullySent = false
        try {
            transport.write(bytes); fullySent = true
            val reply = withTimeout(timeout) { deferred.await() }
            check(session == generation) { "连接已变更，结果未知" }
            if (reply.text("evt") == "error") {
                val code = reply.text("code")
                val description = when (code) {
                    "NEED_MFA" -> "需要重新验证浏览器指纹"
                    "OUT_OF_WINDOW" -> "当前不在设备允许的操作时间内"
                    "NOT_READY" -> "设备尚未就绪，请检查配置和授权"
                    "INVALID_PARAM" -> "设备拒绝参数，请检查输入"
                    else -> "设备未能完成操作，请到诊断页检查状态"
                }
                error(description)
            }
            return reply
        } catch (e: Exception) {
            // A partial write leaves a half-line in firmware: always clear via reconnect.
            if (!fullySent && transport.state.value.connected) transport.disconnect("发送中断，请重连后查询状态")
            throw e
        } finally { if (pending?.first == id) pending = null; bytes.fill(0) }
    }
    fun connect(device: NearbyDevice) {
        if (mutable.value.busy) return
        connecting?.cancel()
        connecting = scope.launch {
            mutable.update { it.copy(busy = true, ready = false, action = "连接设备", message = "正在建立设备控制会话", error = false) }
            try {
                for (attempt in 0..2) {
                    try { transport.connect(device); break }
                    catch (e: CancellationException) { throw e }
                    catch (e: Exception) {
                        if (attempt == 2 || e is SecurityException) throw e
                        mutable.update { it.copy(message = "连接重试 ${attempt + 1}/2，请保持设备靠近") }
                        delay(1500)
                    }
                }
                val info = request("hello").obj("data")
                check((info["proto"] as? JsonPrimitive)?.intOrNull == 1 && info.text("fw") != "—" && info.text("btMac") != "—") { "设备协议不受支持，已停止控制" }
                val maxCommand = (info["maxCommandBytes"] as? JsonPrimitive)?.intOrNull?.coerceIn(1, 4095) ?: 511
                mutable.update { it.copy(firmware = info.text("fw"), mac = info.text("btMac"), maxCommandBytes = maxCommand) }
                refreshInternal()
                mutable.update { it.copy(ready = true, message = "设备已连接，可以开始配置或控制", error = false) }
                transport.ready()
            } catch (e: Exception) {
                transport.disconnect("连接未完成，请重试")
                mutable.update { it.copy(message = safeError(e), error = true) }
            } finally { mutable.update { it.copy(busy = false, action = "") } }
        }
    }
    fun disconnect() { connecting?.cancel(); transport.disconnect(); mutable.update { it.copy(message = "已断开连接。设备的自动调度仍由固件负责。") } }
    private fun safeError(e: Exception) = when (e) {
        is TimeoutCancellationException -> "等待超时，结果未知。请重新连接并查看设备状态，避免重复提交。"
        is SecurityException -> "蓝牙权限被撤销，请重新授予权限后连接。"
        is CancellationException -> "操作已中断，设备执行结果未知。"
        else -> e.message?.takeIf { !it.contains("{") && !it.contains("http") }?.take(160) ?: "操作未完成，请检查连接与配置。"
    }
    fun notice(message: String) { mutable.update { it.copy(message = message, error = true) } }
    fun execute(title: String, op: String, fields: JsonObject = JsonObject(emptyMap()), timeout: Long = 15000, savedStep: String? = null, done: () -> Unit = {}) {
        if (!mutable.value.ready) { notice("请先在设备面板连接 MoonCheck。"); return }
        if (!lock.tryLock()) { notice("设备正在处理上一项操作，请等待结果。"); return }
        if (mutable.value.busy) { lock.unlock(); return }
        scope.launch {
            mutable.update { it.copy(busy = true, action = title, error = false, message = "$title · 等待设备回执") }
            var acknowledged = false
            try {
                val response = request(op, fields, timeout)
                acknowledged = true
                when (op) {
                    "status" -> applyStatus(response.obj("data"))
                    "account.list", "wx.status" -> accountsInternal()
                    "wifi.scan" -> {
                        val data = response["data"]
                        val list = if (data is JsonArray) data else (data as? JsonObject)?.get("aps") as? JsonArray
                        val names = list?.mapNotNull { (it as? JsonObject)?.text("ssid", "")?.takeIf(String::isNotBlank) }?.distinct().orEmpty()
                        mutable.update { it.copy(networks = names) }
                    }
                    "factory.reset", "reboot" -> { transport.disconnect("设备正在重启，请重新扫描连接") }
                    "account.add", "account.remove", "account.clear", "account.enable", "wx.import" -> accountsInternal()
                }
                if (op.startsWith("notify.")) notificationsInternal()
                if (savedStep != null) mutable.update { it.copy(configured = it.configured + savedStep) }
                val detail = when (op) {
                    "test.http" -> {
                        val status = (response.obj("data")["status"] as? JsonPrimitive)?.intOrNull
                        if (status == 204) "网络检查返回 HTTP 204，设备网络已放行。"
                        else "网络检查返回 HTTP ${status ?: "未知"}，可能需要校园网认证，请查看帮助。"
                    }
                    "test.time" -> "设备时间检查已完成，请刷新状态核对校时与时间。"
                    "logs" -> "日志已读取。为保护凭据，原始文本不展示；请使用状态与诊断检查。"
                    "wx.test" -> "微信取票测试通过，未提交打卡。"
                    "wifi.scan" -> "WiFi 扫描完成，可在配置页选择网络。"
                    else -> "$title · 设备已确认完成"
                }
                mutable.update { it.copy(message = detail, error = false, events = (listOf(detail) + it.events).take(20)) }
                done()
                if (op.endsWith(".set") || op in setOf("test.checkin", "test.multi")) {
                    try { refreshInternal() } catch (_: Exception) {
                        transport.disconnect("状态刷新未完成，请重新连接")
                        mutable.update { it.copy(message = "$title 已确认；状态刷新失败，请重连后查询，避免重复提交。", error = true) }
                    }
                }
                if (transport.state.value.connected) transport.ready()
            } catch (e: Exception) {
                // Reconnect before another request: stale fragments/results cannot be mistaken for new ones.
                transport.disconnect("操作未完成，请重连后核验状态")
                mutable.update { it.copy(message = if (acknowledged) "$title 已由设备确认，但后续状态查询未完成，请重连核对。" else safeError(e), error = true) }
            } finally { mutable.update { it.copy(busy = false, action = "") }; lock.unlock() }
        }
    }
    private suspend fun refreshInternal() { applyStatus(request("status").obj("data")); accountsInternal(); notificationsInternal() }
    private suspend fun notificationsInternal() {
        val data = request("notify.status").obj("data")
        val channels = (data["channels"] as? JsonArray)?.mapNotNull {
            val value = it as? JsonObject ?: return@mapNotNull null
            NotifyChannel(value.text("label"), value.flag("on"), value.text("accounts"))
        }.orEmpty()
        mutable.update { it.copy(notifyEnabled = data.flag("on"), channels = channels) }
    }
    private suspend fun accountsInternal() {
        val accounts = request("account.list")["data"] as? JsonArray ?: JsonArray(emptyList())
        val slots = (request("wx.status").obj("data")["slots"] as? JsonArray)?.mapNotNull {
            val value = it as? JsonObject
            if (value?.flag("present") == true) (value["slot"] as? JsonPrimitive)?.intOrNull else null
        }?.toSet().orEmpty()
        mutable.update { state -> state.copy(slots = slots, accounts = accounts.mapIndexedNotNull { index, entry ->
            val account = entry as? JsonObject ?: return@mapIndexedNotNull null
            Account(account.text("label"), account.text("user"), account.flag("enabled"), index, index in slots)
        }) }
    }
    private fun applyStatus(data: JsonObject) {
        val net = data.obj("net"); val cfg = data.obj("cfg"); val schedule = cfg.obj("schedule"); val last = data.obj("last")
        mutable.update { it.copy(
            wifi = cfg.text("wifiSsid"), wifiConnected = net.text("state") == "connected",
            synced = net.flag("timeSynced"), time = net.text("deviceTime"),
            auto = schedule.flag("autoDaily"), at = schedule.text("checkinAt", "22:00"), end = schedule.text("windowEnd", "23:30"),
            lastDate = last.text("date"), lastOk = if (last.text("date", "").isBlank()) null else last.flag("ok")
        ) }
    }
}
