package io.mooncheck.app

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.runtime.*
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import io.mooncheck.app.ui.*

class MainActivity : ComponentActivity() {
    private val vm: ControlViewModel by viewModels()
    private val permissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { grants ->
        if (grants.values.all { it }) scanWithPermission()
        else vm.repository.notice("需要附近设备权限才能连接。可在系统设置中为 MoonCheck 开启权限；帮助内容仍可离线查看。")
    }
    private val bluetooth = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { scanWithPermission(false) }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            val link by vm.transport.state.collectAsStateWithLifecycle()
            val state by vm.repository.state.collectAsStateWithLifecycle()
            // Secure screens while connected; credential forms are also protected separately.
            DisposableEffect(link.connected) {
                if (link.connected) window.addFlags(WindowManager.LayoutParams.FLAG_SECURE)
                else window.clearFlags(WindowManager.LayoutParams.FLAG_SECURE)
                onDispose { }
            }
            MoonTheme { MoonApp(vm, state, link, ::scanWithPermission) }
        }
    }
    private fun scanWithPermission(promptBluetooth: Boolean = true) {
        val required = if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
            else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        if (required.any { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }) { permissions.launch(required); return }
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null) { vm.repository.notice("这台设备不支持蓝牙，请使用具备 BLE 的安卓手机。"); return }
        if (!adapter.isEnabled) {
            if (promptBluetooth) bluetooth.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            else vm.repository.notice("蓝牙未开启，请开启后重新扫描。")
            return
        }
        val location = getSystemService(LocationManager::class.java)
        val locationEnabled = if (Build.VERSION.SDK_INT >= 28) location.isLocationEnabled
            else location.isProviderEnabled(LocationManager.GPS_PROVIDER) || location.isProviderEnabled(LocationManager.NETWORK_PROVIDER)
        if (Build.VERSION.SDK_INT < 31 && !locationEnabled) {
            vm.repository.notice("此 Android 版本扫描 BLE 需要开启系统定位开关。应用不会读取手机坐标。")
            return
        }
        vm.transport.scan()
    }
    override fun onStop() {
        super.onStop()
        if (!isChangingConfigurations) vm.background()
    }
}
