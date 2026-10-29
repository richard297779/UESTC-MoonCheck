package io.mooncheck.app.ui

import io.mooncheck.app.data.Account
import io.mooncheck.app.data.DeviceSnapshot
import kotlinx.serialization.json.*
import org.junit.Assert.*
import org.junit.Test

class AccountCommandsTest {
    private val a=Account("A","fixture-01",true,0,false)
    private val b=Account("B","fixture-02",false,1,false)
    private val state=DeviceSnapshot(accountRevision=22,capabilities=setOf("account-v2","selected-execution"),accounts=listOf(a,b))
    @Test fun editOmitsEmptyPasswordFingerprintAndCoordinates() {
        val result=AccountCommands.payload(Forms.accountEdit,mapOf("_revision" to "22","_target" to "fixture-01","addr" to " new address "),state)
        assertEquals("A",result["target"]!!.jsonPrimitive.content)
        assertEquals(setOf("addr"),result["fields"]!!.jsonObject.keys)
        assertEquals(" new address ",result["fields"]!!.jsonObject["addr"]!!.jsonPrimitive.content)
    }
    @Test fun createRequiresFingerprintAndFreshRevision() {
        val values=mapOf("_revision" to "22","label" to "C","user" to "fixture-03","pass" to "fixture")
        assertThrows(Exception::class.java) { AccountCommands.payload(Forms.accountCreate,values,state) }
        val complete=values+("bfp" to "0123456789abcdef0123456789abcdef")
        assertTrue(AccountCommands.payload(Forms.accountCreate,complete,state)["create"]!!.jsonPrimitive.boolean)
        assertThrows(Exception::class.java) { AccountCommands.payload(Forms.accountCreate,complete,state.copy(accountRevision=23)) }
    }
    @Test fun selectedExecutionRejectsDisabledOrEmptyAndUsesLabels() {
        val form=CommandForm("login","test.login","",emptyList())
        for(labels in listOf("","B","A,A"))assertThrows(Exception::class.java) { AccountCommands.payload(form,mapOf("_revision" to "22","_targets" to labels),state) }
        val result=AccountCommands.payload(form,mapOf("_revision" to "22","_targets" to "A"),state)
        assertEquals(listOf(JsonPrimitive("A")),result["targets"]!!.jsonArray)
    }
    @Test fun wxResolvesTargetAndRejectsAmbiguousStudentIds() {
        val form=Forms.wxTest.copy(inputs=listOf(Input("_target","target")))
        val result=AccountCommands.payload(form,mapOf("_revision" to "22","_target" to "B"),state)
        assertEquals(1,result["slot"]!!.jsonPrimitive.int)
        assertThrows(Exception::class.java) { AccountCommands.resolve(listOf(a,b.copy(user=a.user)),a.user) }
    }
}
