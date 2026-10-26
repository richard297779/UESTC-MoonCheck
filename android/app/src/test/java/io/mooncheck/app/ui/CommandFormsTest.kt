package io.mooncheck.app.ui

import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test

class CommandFormsTest {
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
