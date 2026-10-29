package io.mooncheck.app.ui

import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test

class CommandFormsTest {
    @Test fun accountAcceptsOwnCoordinatesOrInheritsDevicePosition() {
        val base = mapOf("label" to "A01", "user" to "fixture-user", "pass" to "fixture-password", "bfp" to "", "lat" to "", "lng" to "", "addr" to "")
        val inherited = Forms.payload(Forms.account, base)
        assertFalse("lat" in inherited)
        assertFalse("lng" in inherited)
        val addressOnly = Forms.payload(Forms.account, base + ("addr" to "测试 地址 A"))
        assertFalse("lat" in addressOnly)
        assertEquals("测试 地址 A", addressOnly["addr"]!!.jsonPrimitive.content)
        val own = Forms.payload(Forms.account, base + mapOf("lat" to "31.2345", "lng" to "104.5678", "addr" to " 测试  地址 "))
        assertEquals(31.2345, own["lat"]!!.jsonPrimitive.double, 0.0)
        assertEquals(104.5678, own["lng"]!!.jsonPrimitive.double, 0.0)
        assertEquals(" 测试  地址 ", own["addr"]!!.jsonPrimitive.content)
    }
    @Test fun accountRejectsPartialOrInvalidCoordinates() {
        val base = mapOf("label" to "A01", "user" to "fixture-user", "pass" to "fixture-password", "bfp" to "")
        for (values in listOf(mapOf("lat" to "31"), mapOf("lng" to "104"), mapOf("lat" to "NaN", "lng" to "104"),
            mapOf("lat" to "91", "lng" to "104"), mapOf("lat" to "31", "lng" to "181"), mapOf("lat" to "0", "lng" to "104"))) {
            assertThrows(Exception::class.java) { Forms.payload(Forms.account, base + values) }
        }
    }
    @Test fun openWifiAndSpacesArePreserved() {
        val payload = Forms.payload(Forms.wifi, mapOf("ssid" to "room wifi", "pass" to ""))
        assertEquals("", payload["pass"]!!.jsonPrimitive.content)
        assertEquals("room wifi", payload["ssid"]!!.jsonPrimitive.content)
    }
    @Test fun coordinatesMustBeFiniteAndWithinRange() {
        for (lat in listOf("NaN", "Infinity", "91", "0")) assertThrows(Exception::class.java) {
            Forms.payload(Forms.location, mapOf("lat" to lat, "lng" to "103.5"))
        }
    }
    @Test fun rejectsInvalidTimeAndReversedWindow() {
        for ((at, end) in listOf("25:00" to "23:30", "22:30" to "22:00", "20:00" to "23:30")) assertThrows(Exception::class.java) {
            Forms.payload(Forms.schedule, mapOf("at" to at, "window_end" to end, "jitter_s" to "30"))
        }
    }
    @Test fun notificationPauseIsBooleanFalse() {
        val form = CommandForm("pause", "notify.enable", "", listOf(Input("label", "label"), Input("on", "on", kind = Kind.Choice, choices = listOf("1", "0"))))
        assertFalse(Forms.payload(form, mapOf("label" to "A01", "on" to "0"))["on"]!!.jsonPrimitive.boolean)
    }
    @Test fun resetNeedsExactConfirmation() {
        assertThrows(IllegalArgumentException::class.java) { Forms.payload(Forms.reset, mapOf("confirm" to "yes")) }
        assertEquals("YES", Forms.payload(Forms.reset, mapOf("confirm" to "YES"))["confirm"]!!.jsonPrimitive.content)
    }
    @Test fun validatesCredentialSlotAndEpoch() {
        val valid = mapOf("slot" to "0", "openid" to "test-only", "access" to "test-only", "refresh" to "test-only", "expiresAt" to "0")
        assertEquals(0L, Forms.payload(Forms.wx, valid)["expiresAt"]!!.jsonPrimitive.long)
        assertThrows(Exception::class.java) { Forms.payload(Forms.wx, valid + ("slot" to "6")) }
        assertThrows(Exception::class.java) { Forms.payload(Forms.wx, valid + ("expiresAt" to "-1")) }
    }
}
