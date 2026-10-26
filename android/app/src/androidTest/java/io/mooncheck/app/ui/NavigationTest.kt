package io.mooncheck.app.ui

import android.app.Application
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import io.mooncheck.app.ble.LinkState
import io.mooncheck.app.ble.NearbyDevice
import io.mooncheck.app.data.DeviceSnapshot
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

/** UI fixtures never connect to a real device or submit a command. */
@RunWith(AndroidJUnit4::class)
class NavigationTest {
    @get:Rule val compose = createComposeRule()
    private fun launch(connected: Boolean = false) {
        val vm = ControlViewModel(ApplicationProvider.getApplicationContext<Application>())
        compose.setContent { MoonTheme {
            MoonApp(vm, DeviceSnapshot(ready = connected), LinkState(connected = connected,
                device = NearbyDevice("WANAN-DEMO", "00:00:00:00:00:00", -45)), {})
        } }
    }
    @Test fun offlineHelpSearchWorks() {
        launch()
        compose.onNodeWithText("帮助", useUnmergedTree = true).performClick()
        compose.onNodeWithText("随时有帮助").assertIsDisplayed()
        compose.onNode(hasSetTextAction()).performTextInput("微信授权与到期时间")
        compose.onNode(hasText("微信授权与到期时间") and hasSetTextAction().not()).assertIsDisplayed().performClick()
        compose.onNodeWithText("每个账号需要本人", substring = true).assertIsDisplayed()
    }
    @Test fun configurationValidatesInputBeforeSending() {
        launch(true)
        compose.onNodeWithText("配置", useUnmergedTree = true).performClick()
        compose.onNodeWithText("连接 WiFi").performClick()
        compose.onNodeWithText("保存到设备").performScrollTo().performClick()
        compose.onNodeWithText("请填写WiFi 名称").assertIsDisplayed()
    }
    @Test fun realSubmissionRequiresConfirmation() {
        launch(true)
        compose.onNodeWithText("手动提交打卡").performScrollTo().performClick()
        compose.onNodeWithText("提交本次打卡？").assertIsDisplayed()
        compose.onNodeWithText("取消").performClick()
        compose.onNodeWithText("提交本次打卡？").assertDoesNotExist()
    }
}
