package io.mooncheck.app.protocol

import io.mooncheck.app.BuildConfig

import java.util.Base64
import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test

class ProtocolCodecTest {
    private fun chunk(index: Int, total: Int, bytes: ByteArray) = buildJsonObject {
        put("seq", index); put("of", total); put("b64", Base64.getEncoder().encodeToString(bytes))
    }.toString().plus("\n").toByteArray()
    @Test fun productionFirmwareStatusSurvivesSmallMtuChunks() {
        val text = javaClass.getResource("/firmware-status.json")!!.readText()
        val expected = ProtocolCodec.json.parseToJsonElement(text).jsonObject
        val bytes = expected.toString().toByteArray(Charsets.UTF_8)
        val pieces = bytes.asList().chunked(96).map { it.toByteArray() }
        val assembler = FrameAssembler()
        val decoded = mutableListOf<JsonObject>()
        pieces.forEachIndexed { index, piece ->
            chunk(index, pieces.size, piece).asList().chunked(17).forEach { packet -> decoded += assembler.feed(packet.toByteArray()) }
        }
        assertEquals(expected, decoded.single())
        assertEquals(BuildConfig.VERSION_NAME, decoded.single().obj("data").text("fw"))
        assertEquals(220000L, decoded.single().obj("data").obj("runtime")["heapPeakUsedBytes"]!!.jsonPrimitive.long)
        assertTrue(decoded.single().obj("data").obj("net").text("ssid").contains("宿舍"))
    }
    @Test fun requestPreservesSpacesAndEscapes() {
        val bytes = ProtocolCodec.request(7, "wifi.set", buildJsonObject { put("ssid", "宿舍 WiFi"); put("pass", " p\"a\\ss ") })
        val frame = ProtocolCodec.decode(bytes)
        assertEquals("宿舍 WiFi", frame.text("ssid")); assertEquals(" p\"a\\ss ", frame.text("pass"))
        assertEquals(7, frame["id"]!!.jsonPrimitive.int)
        assertEquals(1, bytes.count { it == 10.toByte() })
    }
    @Test fun requestCannotOverrideIdOrOperation() {
        val frame = ProtocolCodec.decode(ProtocolCodec.request(1, "status", buildJsonObject { put("id", 77); put("op", "factory.reset") }))
        assertEquals("status", frame.text("op")); assertEquals("1", frame.text("id"))
    }
    @Test fun requestLimitIncludesNewlineAndUtf8Bytes() {
        val overhead = ProtocolCodec.request(1, "x", buildJsonObject { put("v", "") }).size
        val exact = ProtocolCodec.request(1, "x", buildJsonObject { put("v", "a".repeat(4096 - overhead)) })
        assertEquals(4096, exact.size)
        assertThrows(IllegalArgumentException::class.java) { ProtocolCodec.request(1, "x", buildJsonObject { put("v", "中".repeat(1400)) }) }
    }
    @Test fun packetLimitUsesBytesAndOneNewline() {
        val original = ProtocolCodec.request(1, "x", buildJsonObject { put("v", "中文".repeat(100)) })
        val packets = ProtocolCodec.packets(original, 60)
        assertTrue(packets.all { it.size <= 57 })
        assertArrayEquals(original, packets.fold(byteArrayOf()) { bytes, part -> bytes + part })
        assertThrows(IllegalArgumentException::class.java) { ProtocolCodec.packets(original, 23) }
    }
    @Test fun handlesSplitLinesAndMultipleLines() {
        val parser = FrameAssembler()
        assertTrue(parser.feed("{\"id\":".toByteArray()).isEmpty())
        val results = parser.feed("1}\n{\"evt\":\"progress\"}\n".toByteArray())
        assertEquals(2, results.size); assertEquals("1", results[0].text("id"))
    }
    @Test fun decodesChineseOnlyAfterAllBase64BytesJoined() {
        val full = "{\"id\":1,\"msg\":\"中文结果\"}".toByteArray()
        val cut = full.indexOfFirst { it < 0 } + 1
        val parser = FrameAssembler()
        assertTrue(parser.feed(chunk(0, 2, full.copyOfRange(0, cut))).isEmpty())
        assertEquals("中文结果", parser.feed(chunk(1, 2, full.copyOfRange(cut, full.size))).single().text("msg"))
    }
    @Test fun toleratesReorderingAndIdenticalDuplicates() {
        val full = "{\"id\":9,\"msg\":\"ok\"}".toByteArray()
        val parser = FrameAssembler()
        val first = chunk(0, 3, full.copyOfRange(0, 5))
        parser.feed(first); parser.feed(first)
        parser.feed(chunk(2, 3, full.copyOfRange(10, full.size)))
        assertEquals("9", parser.feed(chunk(1, 3, full.copyOfRange(5, 10))).single().text("id"))
    }
    @Test fun independentEventsDoNotCorruptChunks() {
        val parser = FrameAssembler(); val bytes = "{\"id\":2}".toByteArray()
        parser.feed(chunk(0, 2, bytes.copyOfRange(0, 4)))
        assertEquals("progress", parser.feed("{\"evt\":\"progress\"}\n".toByteArray()).single().text("evt"))
        assertEquals("2", parser.feed(chunk(1, 2, bytes.copyOfRange(4, bytes.size))).single().text("id"))
    }
    @Test fun rejectsConflictAndRecoversForNextMessage() {
        val parser = FrameAssembler(); parser.feed(chunk(0, 2, "abc".toByteArray()))
        assertThrows(IllegalArgumentException::class.java) { parser.feed(chunk(0, 2, "xyz".toByteArray())) }
        assertEquals("1", parser.feed("{\"id\":1}\n".toByteArray()).single().text("id"))
    }
    @Test fun missingChunksExpireWithoutNewNotifications() {
        var now = 0L; val parser = FrameAssembler { now }
        parser.feed(chunk(0, 2, "abc".toByteArray())); now = 5001
        assertThrows(IllegalStateException::class.java) { parser.expire() }
        assertEquals("4", parser.feed("{\"id\":4}\n".toByteArray()).single().text("id"))
    }
    @Test fun boundsAndMalformedInputsAreRejected() {
        val parser = FrameAssembler()
        assertThrows(IllegalArgumentException::class.java) { parser.feed(chunk(0, 2049, byteArrayOf())) }
        assertThrows(IllegalArgumentException::class.java) { parser.feed(chunk(1, 2, byteArrayOf())) }
        assertThrows(IllegalArgumentException::class.java) { parser.feed("not json\n".toByteArray()) }
        assertThrows(IllegalArgumentException::class.java) { parser.feed(chunk(0, 1, ByteArray(65537))) }
        assertThrows(IllegalArgumentException::class.java) { parser.feed(byteArrayOf(0xff.toByte(), 10)) }
    }
    @Test fun legacyStringChunksAndUnknownFieldsRemainReadable() {
        val parser = FrameAssembler()
        val one = buildJsonObject { put("seq", 0); put("of", 2); put("chunk", "{\"id\":") }
        val two = buildJsonObject { put("seq", 1); put("of", 2); put("chunk", "1,\"future\":true}") }
        parser.feed((one.toString() + "\n").toByteArray())
        assertTrue(parser.feed((two.toString() + "\n").toByteArray()).single().flag("future"))
    }
}
