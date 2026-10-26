package io.mooncheck.app.data

import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test

class RuntimeStatusTest {
    private val json = Json
    @Test fun parsesDevicePeakAndActualNetworkWithoutEstimating() {
        val runtime = json.parseToJsonElement("""{"boardId":"c3-devkitm-1-4mb","heapTotalBytes":200000,"heapFreeBytes":100000,"heapMinFreeBytes":30000,"heapPeakUsedBytes":170000,"psramTotalBytes":0,"uptimeSeconds":90000}""").jsonObject
        val net = json.parseToJsonElement("""{"ip":"192.0.2.1","rssi":-57}""").jsonObject
        val value = RuntimeStatus.parse(runtime, net, JsonObject(emptyMap()))
        assertEquals(170000L, value.peak); assertEquals(0L, value.psramTotal)
        assertEquals(-57L, value.rssi); assertEquals(90000L, value.uptime)
    }
    @Test fun oldFirmwareUsesFreeHeapOnlyAndKeepsPeakUnknown() {
        val empty = JsonObject(emptyMap())
        val value = RuntimeStatus.parse(empty, empty, buildJsonObject { put("freeHeapKB", 40) })
        assertEquals(40960L, value.free); assertNull(value.peak); assertNull(value.psramTotal)
    }
    @Test fun malformedAndNegativeMetricsDoNotBecomeRealValues() {
        val data = json.parseToJsonElement("""{"heapFreeBytes":-3,"heapPeakUsedBytes":"invalid"}""").jsonObject
        val value = RuntimeStatus.parse(data, JsonObject(emptyMap()), JsonObject(emptyMap()))
        assertNull(value.free); assertNull(value.peak)
    }
    @Test fun logPrivacyHidesQuotedSecretsUrlsAndOpaqueValues() {
        val logs = """[I] wifi connected
{"password": "private value with spaces"}
ticket=abcd
https://example.invalid/private
00112233445566778899aabbccddeeff
user = 12345678
[I] net_task ready"""
        val safe = LogPrivacy.sanitize(logs)
        assertTrue(LogPrivacy.sanitize("{\"pwd\":\"short\"}\nCASTGC=short\n{\"user\":\"abc\"}").all { it == "[敏感日志已隐藏]" })
        assertEquals(7, safe.size)
        assertEquals(5, safe.count { it == "[敏感日志已隐藏]" })
        assertEquals("[I] wifi connected", safe.first()); assertEquals("[I] net_task ready", safe.last())
    }
    @Test fun logsAreBoundedAndEmptyReplyIsEmpty() {
        assertTrue(LogPrivacy.sanitize("").isEmpty())
        assertEquals(60, LogPrivacy.sanitize((1..100).joinToString("\n") { "line $it" }).size)
        assertEquals(512, LogPrivacy.sanitize(". ".repeat(1000)).single().length)
    }
}
