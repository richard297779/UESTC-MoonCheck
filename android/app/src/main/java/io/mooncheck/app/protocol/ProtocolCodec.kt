package io.mooncheck.app.protocol

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.util.Base64
import kotlinx.serialization.json.*

object ProtocolCodec {
    val json = Json { ignoreUnknownKeys = true }
    fun request(id: Int, op: String, fields: JsonObject = JsonObject(emptyMap())): ByteArray {
        require(id > 0 && op.isNotBlank()) { "请求参数无效" }
        val frame = buildJsonObject {
            fields.forEach { (key, value) -> if (key != "id" && key != "op") put(key, value) }
            put("id", id); put("op", op)
        }
        return (frame.toString() + "\n").toByteArray(Charsets.UTF_8).also {
            require(it.size <= 4096) { "配置超过设备 4096 字节限制，请缩短内容" }
        }
    }
    fun packets(bytes: ByteArray, mtu: Int): List<ByteArray> {
        require(mtu >= 60) { "当前固件要求 MTU 至少为 60" }
        val size = minOf(mtu - 3, 480)
        return bytes.asList().chunked(size).map { it.toByteArray() }
    }
    fun decode(bytes: ByteArray): JsonObject {
        val text = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString()
        return json.parseToJsonElement(text).jsonObject
    }
}

/** The wire protocol has no chunk group ID. Never combine overlapping groups. */
class FrameAssembler(private val clock: () -> Long = { System.nanoTime() / 1_000_000 }) {
    private val line = ByteArrayOutputStream()
    private var pieces = sortedMapOf<Int, ByteArray>()
    private var total = 0
    private var bytes = 0
    private var touched = 0L
    fun reset() { line.reset(); pieces.clear(); total = 0; bytes = 0; touched = 0 }
    fun expire() {
        if ((total > 0 || line.size() > 0) && clock() - touched > 5000) {
            reset(); error("设备回执不完整，请重新查询状态")
        }
    }
    fun feed(input: ByteArray): List<JsonObject> {
        try {
            expire()
            val result = mutableListOf<JsonObject>()
            for (byte in input) {
                touched = clock()
                if (byte == 10.toByte()) {
                    if (line.size() == 0) continue
                    val frame = ProtocolCodec.decode(line.toByteArray()); line.reset()
                    if (frame.containsKey("seq") && frame.containsKey("of")) {
                        val seq = frame["seq"]!!.jsonPrimitive.int
                        val count = frame["of"]!!.jsonPrimitive.int
                        require(count in 1..2048 && seq in 0 until count) { "分片序号无效" }
                        val piece = when {
                            "b64" in frame -> Base64.getDecoder().decode(frame["b64"]!!.jsonPrimitive.content)
                            "chunk" in frame -> frame["chunk"]!!.jsonPrimitive.content.toByteArray(Charsets.UTF_8)
                            else -> error("分片缺少数据")
                        }
                        if (seq == 0 && total > 0) {
                            // A duplicate first packet is allowed only if exactly identical.
                            require(total == count && pieces[0]?.contentEquals(piece) == true) { "回执分片组冲突" }
                        }
                        if (total == 0) { require(seq == 0) { "缺少首个分片" }; total = count }
                        require(total == count) { "回执分片数量不一致" }
                        val old = pieces[seq]
                        require(old == null || old.contentEquals(piece)) { "重复分片内容冲突" }
                        if (old == null) { pieces[seq] = piece; bytes += piece.size }
                        require(bytes <= 65536) { "回执超出安全长度" }
                        if (pieces.size == total) {
                            val full = ByteArrayOutputStream()
                            for (index in 0 until total) full.write(pieces.getValue(index))
                            result += ProtocolCodec.decode(full.toByteArray())
                            pieces.clear(); total = 0; bytes = 0
                        }
                    } else result += frame
                } else {
                    require(line.size() < 65536) { "回执行过长" }
                    line.write(byte.toInt())
                }
            }
            return result
        } catch (exception: Exception) { reset(); throw IllegalArgumentException("回执格式错误或不完整，请重新查询", exception) }
    }
}

fun JsonObject.text(key: String, default: String = "—") = (this[key] as? JsonPrimitive)?.contentOrNull ?: default
fun JsonObject.obj(key: String) = this[key] as? JsonObject ?: JsonObject(emptyMap())
fun JsonObject.flag(key: String) = (this[key] as? JsonPrimitive)?.booleanOrNull == true
