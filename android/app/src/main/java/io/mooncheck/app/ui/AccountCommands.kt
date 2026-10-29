package io.mooncheck.app.ui

import io.mooncheck.app.data.Account
import io.mooncheck.app.data.DeviceSnapshot
import kotlinx.serialization.json.*

/** Explicit snapshots bind a reviewed form to the device account mapping. */
object AccountCommands {
    val executionOps=setOf("test.login","test.chain","test.checkin","test.multi")
    fun resolve(accounts: List<Account>, query: String): Account {
        val value=query.trim()
        val matches=accounts.filter { it.label==value || it.slot.toString()==value || it.user==value }
        require(value.isNotEmpty() && matches.size==1) { "账号匹配不唯一，请刷新后输入标签、序号或完整学号" }
        return matches.single()
    }
    fun payload(form: CommandForm, values: Map<String,String>, state: DeviceSnapshot): JsonObject {
        val raw=Forms.payload(form,values)
        val guarded=form.op in executionOps || form.op in setOf("account.add","account.patch","wx.import","wx.test")
        if(!guarded)return raw
        val revision=state.accountRevision
        require(revision!=null && "account-v2" in state.capabilities) { "请升级支持安全账号管理的新固件后使用此功能" }
        require(values["_revision"]?.toLongOrNull()==revision) { "账号表已变化，请关闭表单、刷新后重新选择目标" }
        fun fields()=buildJsonObject { raw.filterKeys { !it.startsWith("_") }.forEach { (k,v)->put(k,v) };form.extra.forEach { (k,v)->put(k,v) } }
        return when(form.op) {
            "account.add","account.patch" -> {
                val creating=form.op=="account.add"
                val changes=fields()
                require(changes.isNotEmpty()) { "没有修改，不会发送空更新" }
                if(creating)require(listOf("label","user","pass","bfp").all { changes[it]?.jsonPrimitive?.content?.isNotBlank()==true }) { "新账号须填写标签、学号、密码和浏览器指纹" }
                val target=if(creating)null else resolve(state.accounts,values["_target"].orEmpty())
                require(target?.primary!=true) { "主账号请使用基础账号设置" }
                buildJsonObject { put("create",creating);put("target",target?.label.orEmpty());put("fields",changes);put("expectedRevision",revision) }
            }
            "wx.import","wx.test" -> {
                val target=resolve(state.accounts,values["_target"].orEmpty())
                buildJsonObject { fields().forEach { (k,v)->put(k,v) };put("slot",target.slot);put("target",target.label);put("expectedRevision",revision) }
            }
            else -> {
                require("selected-execution" in state.capabilities) { "固件尚不支持选择账号执行" }
                val labels=values["_targets"].orEmpty().split(',').filter(String::isNotEmpty)
                require(labels.isNotEmpty() && labels.size<=6 && labels.distinct().size==labels.size) { "请选择一个或多个目标账号" }
                val targets=labels.map { label->state.accounts.singleOrNull { it.label==label } ?: error("目标已变化，请重新选择") }
                require(targets.all { it.enabled }) { "请先明确启用目标账号" }
                buildJsonObject { fields().forEach { (k,v)->put(k,v) };put("targets",JsonArray(labels.map(::JsonPrimitive)));put("expectedRevision",revision) }
            }
        }
    }
}
