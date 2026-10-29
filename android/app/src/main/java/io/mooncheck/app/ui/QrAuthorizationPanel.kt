package io.mooncheck.app.ui

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.unit.dp
import io.mooncheck.app.data.QrAuthorization
import kotlinx.coroutines.*

@Composable
fun QrAuthorizationPanel(slot: String, beforeStart: () -> Unit, fill: (Map<String, String>) -> Unit) {
    var generation by remember { mutableIntStateOf(0) }
    var running by remember { mutableStateOf(false) }
    var image by remember { mutableStateOf<ImageBitmap?>(null) }
    var note by remember { mutableStateOf("可直接获取微信二维码，或手动填写。只填入当前槽位，不自动保存到设备。") }
    val receiver by rememberUpdatedState(fill)
    val scope = rememberCoroutineScope()
    var job by remember { mutableStateOf<Job?>(null) }
    var session by remember { mutableStateOf<QrAuthorization?>(null) }
    fun cancel() {
        generation++
        running = false
        image = null
        session?.close()
        session = null
        job?.cancel()
    }
    DisposableEffect(slot) { onDispose { cancel() } }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text("微信扫码授权", style = MaterialTheme.typography.titleMedium)
        Text("槽位 $slot · 请用本人微信扫描另一块屏幕上的二维码；同一手机无法用相机扫描本屏幕，可改用电脑 BLE 中控显示二维码。不读取已有浏览器登录状态。", style = MaterialTheme.typography.bodySmall)
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = {
                cancel()
                beforeStart()
                val current = generation
                val client = QrAuthorization()
                session = client
                running = true
                note = "正在获取二维码…"
                job = scope.launch {
                    try {
                        image = withContext(Dispatchers.IO) {
                            val bytes = client.start()
                            (BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: error("二维码图片无法显示")).asImageBitmap()
                        }
                        note = "请使用本人微信扫描，并在微信确认授权"
                        withTimeout(240000) {
                            while (isActive && current == generation) {
                                val (status, fields) = withContext(Dispatchers.IO) { client.poll() }
                                if (current != generation) return@withTimeout
                                if (status == 405 && fields != null) {
                                    receiver(fields)
                                    image = null
                                    note = "三个授权字段已填入；请核对槽位后点击核对并继续"
                                    break
                                }
                                if (status == 402 || status == 403) {
                                    image = null
                                    note = "二维码已过期或授权被取消，请刷新"
                                    break
                                }
                                note = if (status == 404) "已扫码，请在微信确认授权" else "等待本人微信扫码授权"
                                delay(1000)
                            }
                        }
                    } catch (error: CancellationException) {
                        if (current == generation) { image = null; note = "授权已取消或超时，请刷新重试" }
                        throw error
                    } catch (_: Exception) {
                        if (current == generation) { image = null; note = "未取得完整授权：检查网络或刷新二维码；也可手动导入" }
                    } finally {
                        client.close()
                        if (current == generation) running = false
                    }
                }
            }) { Text(if (running || image != null) "刷新二维码" else "获取二维码") }
            if (running) TextButton(onClick = { cancel(); note = "授权已取消，未导入设备" }) { Text("取消扫码") }
        }
        image?.let { Image(it, "微信授权二维码", Modifier.fillMaxWidth().height(260.dp)) }
        if (running && image == null) LinearProgressIndicator(Modifier.fillMaxWidth())
        Text(note, style = MaterialTheme.typography.bodySmall)
        Text("二维码不是令牌备份；授权 Cookie 只存在当前表单，刷新后请勿继续使用旧二维码。到期填 0，由固件首次使用前刷新。", style = MaterialTheme.typography.bodySmall)
    }
}
