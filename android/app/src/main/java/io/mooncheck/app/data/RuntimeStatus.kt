package io.mooncheck.app.data

import io.mooncheck.app.protocol.text
import kotlinx.serialization.json.*

/** Byte values come from the device allocator, never estimated from phone memory. */
data class RuntimeStatus(
    val board: String = "—", val chip: String = "—", val uptime: Long? = null,
    val total: Long? = null, val free: Long? = null, val minimum: Long? = null,
    val peak: Long? = null, val largest: Long? = null,
    val psramTotal: Long? = null, val psramFree: Long? = null, val psramMinimum: Long? = null,
    val stackMinimum: Long? = null, val ip: String = "—", val rssi: Long? = null
) {
    companion object {
        fun parse(runtime: JsonObject, net: JsonObject, chip: JsonObject): RuntimeStatus {
            fun JsonObject.number(key: String) = (get(key) as? JsonPrimitive)?.longOrNull?.takeIf { it >= 0 }
            return RuntimeStatus(runtime.text("boardId"), runtime.text("chipModel"), runtime.number("uptimeSeconds"),
                runtime.number("heapTotalBytes"), runtime.number("heapFreeBytes") ?: chip.number("freeHeapKB")?.times(1024),
                runtime.number("heapMinFreeBytes"), runtime.number("heapPeakUsedBytes"), runtime.number("heapLargestBlockBytes"),
                runtime.number("psramTotalBytes"), runtime.number("psramFreeBytes"), runtime.number("psramMinFreeBytes"),
                runtime.number("netTaskStackFreeMinBytes"), net.text("ip"),
                (net["rssi"] as? JsonPrimitive)?.longOrNull?.takeIf { it in -127..0 })
        }
    }
}

object LogPrivacy {
    // Fail closed for unstructured old-firmware logs; no raw log export or persistence.
    private val sensitive = Regex("(?i)pass|pwd|castgc|encrypt|private|key[=:]|cookie|session|authorization|token|ticket|captcha|verifycode|smscode|mfacode|fingerprint|bfp|secret|access|refresh|openid|unionid|sendkey|https?://|user|student|\\b\\d{8,}\\b|[a-zA-Z0-9+/=]{24,}")
    fun sanitize(text: String): List<String> = text.take(24000).takeIf { it.isNotBlank() }.orEmpty().lineSequence().filter(String::isNotBlank).takeLastBounded(60)
        .map { line -> if (sensitive.containsMatchIn(line)) "[敏感日志已隐藏]" else line.filter { it >= ' ' || it == '\t' }.take(512) }
    private fun Sequence<String>.takeLastBounded(count: Int) = toList().takeLast(count)
}
