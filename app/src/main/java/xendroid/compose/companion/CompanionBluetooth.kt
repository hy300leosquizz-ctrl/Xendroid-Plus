package xendroid.compose.companion

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothServerSocket
import android.bluetooth.BluetoothSocket
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.util.Locale
import java.util.UUID

/**
 * Clean-room Bluetooth RFCOMM transport fallback for P2–P4 companion phone controllers
 * (`android.bluetooth.BluetoothServerSocket` / `BluetoothSocket`), sharing the exact
 * HMAC-SHA256 challenge-response handshake and framing of [CompanionProtocol] & [CompanionCodec]
 * so multiplayer works completely offline without Wi-Fi, hotspot, or LAN.
 */
object CompanionBluetooth {

    val SERVICE_UUID: UUID = UUID.fromString("8f360c0e-5865-4e44-926f-696450616431")
    const val SERVICE_NAME = "XenDroidCompanionPad"

    data class PairedDevice(val name: String, val address: String)

    private val MAC_REGEX = Regex("(?i)^(?:bt://)?([0-9a-f]{2}(?::[0-9a-f]{2}){5})$")

    /** Extracts a canonical uppercase `AA:BB:CC:DD:EE:FF` Bluetooth MAC address (accepts optional `bt://` prefix). */
    fun parseMac(input: String): String? =
        MAC_REGEX.matchEntire(input.trim())?.groupValues?.get(1)?.uppercase(Locale.US)

    fun hasConnectPermission(context: Context): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
            context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED

    private fun adapter(context: Context): BluetoothAdapter? =
        runCatching { context.getSystemService(BluetoothManager::class.java)?.adapter }.getOrNull()

    fun isAvailable(context: Context): Boolean =
        hasConnectPermission(context) && adapter(context)?.isEnabled == true

    @SuppressLint("MissingPermission")
    fun pairedDevices(context: Context): List<PairedDevice> {
        if (!hasConnectPermission(context)) return emptyList()
        val bt = adapter(context)?.takeIf { it.isEnabled } ?: return emptyList()
        return runCatching {
            bt.bondedDevices.orEmpty()
                .mapNotNull { dev ->
                    val addr = dev.address?.takeIf { parseMac(it) != null } ?: return@mapNotNull null
                    val label = runCatching { dev.name }.getOrNull()?.trim().orEmpty().ifEmpty { addr }
                    PairedDevice(label, addr.uppercase(Locale.US))
                }
                .sortedBy { it.name.lowercase(Locale.US) }
        }.getOrDefault(emptyList())
    }

    @SuppressLint("MissingPermission")
    fun openServer(context: Context): CompanionHost.StreamServer {
        if (!hasConnectPermission(context)) {
            throw IOException("Bluetooth permission not granted")
        }
        val bt = adapter(context)?.takeIf { it.isEnabled }
            ?: throw IOException("Bluetooth is turned off")
        val serverSocket: BluetoothServerSocket =
            bt.listenUsingRfcommWithServiceRecord(SERVICE_NAME, SERVICE_UUID)
        val hostName = runCatching { bt.name }.getOrNull()?.trim().orEmpty().ifEmpty { "Bluetooth" }
        return object : CompanionHost.StreamServer {
            override val endpointLabel: String = "Bluetooth ($hostName)"
            override fun accept(): CompanionHost.StreamConn {
                val sock = serverSocket.accept()
                return BluetoothConn(sock)
            }
            override fun close() {
                runCatching { serverSocket.close() }
            }
        }
    }

    @SuppressLint("MissingPermission")
    fun connectClientStream(context: Context, macAddress: String): CompanionHost.StreamConn {
        if (!hasConnectPermission(context)) {
            throw IOException("Bluetooth permission not granted")
        }
        val bt = adapter(context)?.takeIf { it.isEnabled }
            ?: throw IOException("Bluetooth is turned off")
        val normalized = parseMac(macAddress) ?: throw IOException("Invalid Bluetooth address")
        runCatching { bt.cancelDiscovery() }
        val device = bt.getRemoteDevice(normalized)
        val socket = device.createRfcommSocketToServiceRecord(SERVICE_UUID)
        try {
            socket.connect()
            return BluetoothConn(socket)
        } catch (e: Exception) {
            runCatching { socket.close() }
            throw e
        }
    }

    private class BluetoothConn(private val socket: BluetoothSocket) : CompanionHost.StreamConn {
        override val inputStream: InputStream get() = socket.inputStream
        override val outputStream: OutputStream get() = socket.outputStream
        override fun close() {
            runCatching { socket.close() }
        }
    }
}
