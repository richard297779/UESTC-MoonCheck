package io.mooncheck.app.ble

import kotlinx.coroutines.flow.StateFlow

interface DeviceTransport {
    val state: StateFlow<LinkState>
    var onBytes: (ByteArray) -> Unit
    var onDisconnect: () -> Unit
    fun scan()
    fun stopScan()
    suspend fun connect(device: NearbyDevice)
    suspend fun write(bytes: ByteArray)
    fun ready()
    fun disconnect(message: String = "未连接")
}
