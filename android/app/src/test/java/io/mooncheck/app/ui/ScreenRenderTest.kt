package io.mooncheck.app.ui

import android.app.Application
import android.graphics.Canvas
import android.graphics.Bitmap
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.activity.ComponentActivity
import androidx.test.core.app.ApplicationProvider
import io.mooncheck.app.ble.LinkState
import io.mooncheck.app.ble.NearbyDevice
import io.mooncheck.app.data.DeviceSnapshot
import io.mooncheck.app.BuildConfig
import java.io.File
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], qualifiers = "zh-rCN-w412dp-h915dp-420dpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class ScreenRenderTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private fun launch(ready: Boolean = false) {
        val vm = ControlViewModel(ApplicationProvider.getApplicationContext<Application>())
        compose.setContent { MoonTheme { MoonApp(vm,
            DeviceSnapshot(ready = ready, firmware = BuildConfig.VERSION_NAME, wifi = "Dorm WiFi", wifiConnected = true,
                synced = true, auto = true, time = "2026-10-26 21:30:00", lastDate = "2026-10-25", lastOk = true),
            LinkState(connected = ready, phase = if (ready) "已连接 · 控制就绪" else "未连接", mtu = 512,
                device = NearbyDevice("WANAN-DEMO", "00:00:00:00:00:00", -45)), {}) } }
    }
    private fun capture(name: String) {
        compose.waitForIdle()
        val file = File("build/reports/screenshots/$name.png"); file.parentFile!!.mkdirs()
        // PixelCopy waits for a real display frame on some Windows hosts. Render
        // the measured Android view directly instead; semantics assertions above
        // still exercise the same production Compose tree.
        compose.runOnIdle {
            val view = compose.activity.window.decorView
            val bitmap = Bitmap.createBitmap(view.width, view.height, Bitmap.Config.ARGB_8888)
            view.draw(Canvas(bitmap))
            file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
            bitmap.recycle()
        }
    }
    @Test fun dashboardRendersWithoutDevice() { launch(); compose.onNodeWithText("晚安，从这里开始").assertIsDisplayed(); capture("01-dashboard-offline") }
    @Test fun configuredDashboardRendersFixture() { launch(true); compose.onNodeWithText("自动计划已开启").assertIsDisplayed(); capture("02-dashboard-demo") }
    @Test fun configurationHasGuidedActions() {
        launch(true); compose.onNodeWithText("配置", useUnmergedTree = true).performClick()
        compose.onNodeWithText("连接 WiFi").assertIsDisplayed(); capture("03-configuration")
        compose.onNodeWithText("连接 WiFi").performClick()
        compose.onNodeWithText("保存到设备").performScrollTo().performClick()
        compose.onNodeWithText("请填写WiFi 名称").assertIsDisplayed()
    }
    @Test fun accountScreenExplainsSlotZero() {
        launch(true); compose.onNodeWithText("账号", useUnmergedTree = true).performClick()
        compose.onNodeWithText("单人配置模式").assertIsDisplayed(); capture("04-accounts")
    }
    @Test fun helpIsSearchableOffline() {
        launch(); compose.onNodeWithText("帮助", useUnmergedTree = true).performClick()
        compose.onNodeWithText("随时有帮助").assertIsDisplayed(); capture("05-help")
        compose.onNode(hasSetTextAction()).performTextInput("微信授权与到期时间")
        compose.onNode(hasText("微信授权与到期时间") and hasSetTextAction().not()).performClick()
        compose.onNodeWithText("每个账号需要本人", substring = true).assertIsDisplayed()
    }
    @Test fun checkinNeedsConfirmation() {
        launch(true); compose.onNodeWithText("手动提交打卡").performScrollTo().performClick()
        compose.onNodeWithText("提交本次打卡？").assertIsDisplayed()
        compose.onNodeWithText("取消").performClick(); compose.onNodeWithText("提交本次打卡？").assertDoesNotExist()
    }
}
