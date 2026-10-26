@file:Suppress("MissingPermission", "DEPRECATION")
package io.mooncheck.app.ble

import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import io.mooncheck.app.protocol.ProtocolCodec
import java.util.UUID
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

data class NearbyDevice(val name: String, val address: String, val rssi: Int)
data class LinkState(val phase: String = "未连接", val connected: Boolean = false,
    val scanning: Boolean = false, val mtu: Int = 23, val device: NearbyDevice? = null,
    val devices: List<NearbyDevice> = emptyList())

class BleTransport(private val context: Context, private val scope: CoroutineScope) : DeviceTransport {
    companion object {
        val SERVICE: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val RX: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
        val TX: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
    private val handler = Handler(Looper.getMainLooper())
    private val manager = context.getSystemService(BluetoothManager::class.java)
    private val mutable = MutableStateFlow(LinkState())
    override val state = mutable.asStateFlow()
    override var onBytes: (ByteArray) -> Unit = {}
    override var onDisconnect: () -> Unit = {}
    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null
    private var gate: CompletableDeferred<Unit>? = null
    private var timeout: Job? = null
    private var scanner: BluetoothLeScanner? = null
    private fun main(action: () -> Unit) { handler.post(action) }
    private fun complete(status: Int) {
        if (status == BluetoothGatt.GATT_SUCCESS) gate?.complete(Unit)
        else gate?.completeExceptionally(IllegalStateException("蓝牙操作失败（$status），请重连"))
    }
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) { main {
            if (!mutable.value.scanning) return@main
            val name = result.scanRecord?.deviceName ?: "未命名设备"
            val serviceMatch = result.scanRecord?.serviceUuids?.any { it.uuid == SERVICE } == true
            if (!serviceMatch && !name.startsWith("WANAN", true)) return@main
            val device = NearbyDevice(name, result.device.address, result.rssi)
            mutable.update { it.copy(devices = (it.devices.filterNot { d -> d.address == device.address } + device).sortedByDescending { d -> d.rssi }) }
        } }
        override fun onScanFailed(errorCode: Int) { main { stopScan(); mutable.update { it.copy(phase = "扫描失败（$errorCode），请检查蓝牙和权限") } } }
    }
    override fun scan() {
        try {
            require(manager?.adapter?.isEnabled == true) { "请先开启手机蓝牙" }
            stopScan()
            scanner = manager.adapter.bluetoothLeScanner ?: error("此手机不支持 BLE 扫描")
            mutable.update { it.copy(scanning = true, devices = emptyList(), phase = if (it.connected) it.phase else "正在寻找附近设备") }
            scanner!!.startScan(null, ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scanCallback)
            timeout = scope.launch { delay(15000); stopScan() }
        } catch (e: Exception) { stopScan(); mutable.update { it.copy(phase = if (e is SecurityException) "请授予附近设备权限" else e.message ?: "扫描失败") } }
    }
    override fun stopScan() {
        timeout?.cancel(); timeout = null
        try { scanner?.stopScan(scanCallback) } catch (_: SecurityException) { }
        scanner = null
        mutable.update { it.copy(scanning = false, phase = if (it.scanning && !it.connected) "扫描已结束，请选择设备" else it.phase) }
    }
    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) { main {
            if (g !== gatt) return@main
            if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED) complete(status)
            else if (newState == BluetoothProfile.STATE_DISCONNECTED || status != BluetoothGatt.GATT_SUCCESS) {
                disconnect("连接已断开（$status），未完成操作结果未知")
            }
        } }
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) { main { if (g === gatt) complete(status) } }
        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) { main {
            if (g !== gatt) return@main
            if (status == BluetoothGatt.GATT_SUCCESS) mutable.update { it.copy(mtu = mtu) }
            complete(status)
        } }
        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) { main { if (g === gatt && descriptor.uuid == CCCD) complete(status) } }
        override fun onCharacteristicWrite(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) { main { if (g === gatt && characteristic.uuid == RX) complete(status) } }
        @Deprecated("Used on Android 8–12")
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) deliver(g, characteristic.uuid, characteristic.value?.clone() ?: byteArrayOf())
        }
        override fun onCharacteristicChanged(g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) { deliver(g, characteristic.uuid, value.clone()) }
    }
    private fun deliver(g: BluetoothGatt, uuid: UUID, value: ByteArray) { main { if (g === gatt && uuid == TX) onBytes(value) } }
    private suspend fun step(label: String, millis: Long = 12000, action: () -> Boolean) {
        mutable.update { it.copy(phase = label) }
        val deferred = CompletableDeferred<Unit>(); gate = deferred
        try {
            check(action()) { "$label：系统拒绝请求" }
            withTimeout(millis) { deferred.await() }
        } finally { if (gate === deferred) gate = null }
    }
    override suspend fun connect(device: NearbyDevice) {
        disconnect(); stopScan()
        mutable.update { it.copy(device = device, mtu = 23) }
        try {
            require(manager?.adapter?.isEnabled == true) { "请先开启手机蓝牙" }
            step("正在连接 ${device.name}", 20000) {
                gatt = manager.adapter.getRemoteDevice(device.address).connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
                gatt != null
            }
            val current = gatt ?: error("设备已断开")
            step("正在发现蓝牙服务") { current.discoverServices() }
            val service = current.getService(SERVICE) ?: error("设备没有 MoonCheck UART 服务")
            rx = service.getCharacteristic(RX) ?: error("缺少控制特征")
            check(rx!!.properties and BluetoothGattCharacteristic.PROPERTY_WRITE != 0) { "设备不支持带响应写入" }
            val tx = service.getCharacteristic(TX) ?: error("缺少通知特征")
            check(tx.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0) { "设备不支持通知" }
            step("正在协商蓝牙 MTU") { current.requestMtu(512) }
            require(mutable.value.mtu >= 60) { "当前固件要求 MTU 至少为 60，请重连或更换手机" }
            check(current.setCharacteristicNotification(tx, true)) { "通知开启失败" }
            val descriptor = tx.getDescriptor(CCCD) ?: error("设备缺少通知描述符")
            step("正在订阅设备回执") {
                if (Build.VERSION.SDK_INT >= 33) current.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothStatusCodes.SUCCESS
                else { descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE; current.writeDescriptor(descriptor) }
            }
            mutable.update { it.copy(connected = true, phase = "正在核验设备身份") }
        } catch (e: Exception) { disconnect("连接失败，请重试"); throw e }
    }
    override suspend fun write(bytes: ByteArray) {
        val current = gatt ?: error("请先连接设备")
        val characteristic = rx ?: error("控制特征尚未就绪")
        for (packet in ProtocolCodec.packets(bytes, mutable.value.mtu)) {
            step("正在发送指令") {
                if (Build.VERSION.SDK_INT >= 33) current.writeCharacteristic(characteristic, packet, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
                else {
                    characteristic.value = packet
                    characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                    current.writeCharacteristic(characteristic)
                }
            }
        }
        mutable.update { it.copy(phase = "等待设备处理") }
    }
    override fun ready() { mutable.update { it.copy(phase = "已连接 · 控制就绪") } }
    override fun disconnect(message: String) {
        stopScan()
        val old = gatt; gatt = null; rx = null
        gate?.completeExceptionally(IllegalStateException("设备已断开，结果未知")); gate = null
        try { old?.disconnect() } catch (_: SecurityException) { }
        try { old?.close() } catch (_: SecurityException) { }
        mutable.update { it.copy(connected = false, phase = message, mtu = 23) }
        onDisconnect()
    }
}
