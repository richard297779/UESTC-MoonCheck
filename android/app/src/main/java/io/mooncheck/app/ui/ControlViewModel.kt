package io.mooncheck.app.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import io.mooncheck.app.ble.BleTransport
import io.mooncheck.app.data.DeviceRepository

class ControlViewModel(application: Application) : AndroidViewModel(application) {
    val transport = BleTransport(application, viewModelScope)
    val repository = DeviceRepository(transport, viewModelScope)
    fun background() { repository.disconnect() }
    override fun onCleared() { transport.disconnect() }
}
