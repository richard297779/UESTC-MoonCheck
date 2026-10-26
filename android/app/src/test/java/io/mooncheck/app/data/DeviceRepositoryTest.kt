@file:OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
package io.mooncheck.app.data

import io.mooncheck.app.ble.*
import io.mooncheck.app.protocol.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.*
import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test

class DeviceRepositoryTest {
    private class Fake : DeviceTransport {
        override val state = MutableStateFlow(LinkState())
        override var onBytes: (ByteArray) -> Unit = {}
        override var onDisconnect: () -> Unit = {}
        val sent = mutableListOf<String>()
        var respond = true
        var wrongId = false
        var failWrite = false
        var longCommands = false
        var omitStatusReplyAfterSave = false
        override fun scan() = Unit
        override fun stopScan() = Unit
        override fun ready() = Unit
        override suspend fun connect(device: NearbyDevice) { state.value = LinkState(connected = true, mtu = 512, device = device) }
        override fun disconnect(message: String) { state.value = state.value.copy(connected = false); onDisconnect() }
        override suspend fun write(bytes: ByteArray) {
            val request = ProtocolCodec.decode(bytes)
            val op = request.text("op"); sent += op
            if (op == "status" && omitStatusReplyAfterSave && "auth.set" in sent) return
            if (failWrite) error("发送中断，请重连")
            if (!respond) return
            val data: JsonElement = when (op) {
                "hello" -> buildJsonObject { put("proto", 1); put("fw", "0.5.1"); put("btMac", "00:00:00:00:00:01"); if (longCommands) put("maxCommandBytes", 4095) }
                "status" -> buildJsonObject { put("net", buildJsonObject { put("state", "connected"); put("timeSynced", true) }) }
                "account.list" -> buildJsonArray { add(buildJsonObject { put("label", "A01"); put("user", "masked"); put("enabled", true) }) }
                "wx.status" -> buildJsonObject { put("slots", buildJsonArray { add(buildJsonObject { put("slot", 0); put("present", true) }) }) }
                else -> JsonObject(emptyMap())
            }
            val response = buildJsonObject { put("id", request["id"]!!.jsonPrimitive.int + if (wrongId) 10 else 0); put("evt", "ok"); put("data", data) }
            onBytes((response.toString() + "\n").toByteArray())
        }
    }
    private val board = NearbyDevice("WANAN-TEST", "00:00:00:00:00:01", -40)
    @Test fun handshakeLoadsStatusAndExplicitAccountOrder() = runTest {
        val fake = Fake(); val repository = DeviceRepository(fake, backgroundScope)
        repository.connect(board); runCurrent()
        assertTrue(repository.state.value.ready)
        assertTrue(repository.state.value.wifiConnected)
        assertEquals(0, repository.state.value.accounts.single().slot)
        assertTrue(repository.state.value.accounts.single().authorized)
        assertEquals(listOf("hello", "status", "account.list", "wx.status", "notify.status"), fake.sent)
    }
    @Test fun timeoutDoesNotReplaySubmissionAndInvalidatesSession() = runTest {
        val fake = Fake(); val repository = DeviceRepository(fake, backgroundScope)
        repository.connect(board); runCurrent(); fake.respond = false
        repository.execute("手动打卡", "test.checkin", timeout = 100)
        runCurrent(); advanceTimeBy(101); runCurrent()
        assertEquals(1, fake.sent.count { it == "test.checkin" })
        assertFalse(repository.state.value.ready)
        assertTrue(repository.state.value.message.contains("结果未知"))
    }
    @Test fun unrelatedIdCannotCompleteRequest() = runTest {
        val fake = Fake(); val repository = DeviceRepository(fake, backgroundScope)
        repository.connect(board); runCurrent(); fake.wrongId = true
        repository.execute("检查", "test.probe", timeout = 100)
        runCurrent(); assertTrue(repository.state.value.busy)
        advanceTimeBy(101); runCurrent()
        assertTrue(repository.state.value.error)
    }
    @Test fun partialWriteDisconnectsAndNeverRetriesCommand() = runTest {
        val fake = Fake(); val repository = DeviceRepository(fake, backgroundScope)
        repository.connect(board); runCurrent(); fake.failWrite = true
        repository.execute("保存", "auth.set"); runCurrent()
        assertFalse(fake.state.value.connected)
        assertEquals(1, fake.sent.count { it == "auth.set" })
        assertFalse(repository.state.value.busy)
    }
    @Test fun busyPreventsSecondCommandAndReconnectClearsPending() = runTest {
        val fake = Fake(); val repository = DeviceRepository(fake, backgroundScope)
        repository.connect(board); runCurrent(); fake.respond = false
        repository.execute("体检", "test.probe"); runCurrent()
        repository.execute("打卡", "test.checkin"); runCurrent()
        assertFalse(fake.sent.contains("test.checkin"))
        repository.disconnect(); runCurrent()
        assertFalse(repository.state.value.ready)
        assertFalse(repository.state.value.busy)
    }
    @Test fun oldFirmwareRejectsLongCredentialsBeforeAnyWrite() = runTest {
        val fake = Fake(); val repository = DeviceRepository(fake, backgroundScope)
        repository.connect(board); runCurrent()
        assertEquals(511, repository.state.value.maxCommandBytes)
        repository.execute("授权导入", "wx.import", buildJsonObject { put("access", "x".repeat(700)) }); runCurrent()
        assertFalse(fake.sent.contains("wx.import"))
        assertTrue(repository.state.value.message.contains("长"))
    }
    @Test fun declaredCapabilityAllowsLongRequest() = runTest {
        val fake = Fake().apply { longCommands = true }; val repository = DeviceRepository(fake, backgroundScope)
        repository.connect(board); runCurrent()
        assertEquals(4095, repository.state.value.maxCommandBytes)
        repository.execute("授权导入", "wx.import", buildJsonObject { put("access", "x".repeat(700)) }); runCurrent()
        assertEquals(1, fake.sent.count { it == "wx.import" })
        assertFalse(repository.state.value.error)
    }
    @Test fun failedRefreshAfterConfirmedSaveDisconnectsWithoutReplay() = runTest {
        val fake = Fake(); val repository = DeviceRepository(fake, backgroundScope)
        repository.connect(board); runCurrent(); fake.omitStatusReplyAfterSave = true
        repository.execute("保存账号", "auth.set"); runCurrent()
        advanceTimeBy(15001); runCurrent()
        assertFalse(repository.state.value.ready)
        assertTrue(repository.state.value.error)
        assertTrue(repository.state.value.message.contains("已确认"))
        assertEquals(1, fake.sent.count { it == "auth.set" })
    }
}
