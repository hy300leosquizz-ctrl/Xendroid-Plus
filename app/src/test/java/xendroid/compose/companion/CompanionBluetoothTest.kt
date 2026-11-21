package xendroid.compose.companion

import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.io.PipedInputStream
import java.io.PipedOutputStream
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import xendroid.compose.gamepad.ControllerSlots

class CompanionBluetoothTest {
    private val closeables = CopyOnWriteArrayList<AutoCloseable>()

    @After fun tearDown() {
        closeables.reversed().forEach { runCatching { it.close() } }
    }

    private fun eventually(what: String, timeoutMs: Long = 3_000L, check: () -> Boolean) {
        val deadline = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < deadline) {
            if (check()) return
            Thread.sleep(10)
        }
        fail("Timed out waiting for $what")
    }

    private class InMemoryRfcommBus : CompanionHost.StreamServer {
        private val pending = LinkedBlockingQueue<CompanionHost.StreamConn>()
        @Volatile private var closed = false

        override val endpointLabel: String = "Bluetooth (Pixel 9)"

        fun connectClient(): CompanionHost.StreamConn {
            if (closed) throw IOException("RFCOMM server closed")
            val c2sIn = PipedInputStream(4096)
            val c2sOut = PipedOutputStream(c2sIn)
            val s2cIn = PipedInputStream(4096)
            val s2cOut = PipedOutputStream(s2cIn)
            val serverEnd = object : CompanionHost.StreamConn {
                override val inputStream: InputStream = c2sIn
                override val outputStream: OutputStream = s2cOut
                override fun close() {
                    runCatching { c2sIn.close() }
                    runCatching { s2cOut.close() }
                }
            }
            val clientEnd = object : CompanionHost.StreamConn {
                override val inputStream: InputStream = s2cIn
                override val outputStream: OutputStream = c2sOut
                override fun close() {
                    runCatching { s2cIn.close() }
                    runCatching { c2sOut.close() }
                }
            }
            pending.put(serverEnd)
            return clientEnd
        }

        override fun accept(): CompanionHost.StreamConn {
            while (!closed) {
                val conn = pending.poll(50, TimeUnit.MILLISECONDS)
                if (conn != null) return conn
            }
            throw IOException("RFCOMM server closed")
        }

        override fun close() {
            closed = true
        }
    }

    @Test fun macAddressParserAcceptsRawAndBtPrefixedAddresses() {
        assertEquals("AA:BB:CC:DD:EE:FF", CompanionBluetooth.parseMac("aa:bb:cc:dd:ee:ff"))
        assertEquals("11:22:33:44:55:66", CompanionBluetooth.parseMac("  bt://11:22:33:44:55:66 "))
        assertNull(CompanionBluetooth.parseMac("192.168.1.20:41234"))
        assertNull(CompanionBluetooth.parseMac("AA:BB:CC:DD:EE"))
        assertNull(CompanionBluetooth.parseMac(""))
    }

    @Test fun streamTransportAuthenticatesAndStreamsButtonsAndRumbleWithoutTcp() {
        val bus = InMemoryRfcommBus().also { closeables += it }
        val slots = ControllerSlots()
        val inputs = CopyOnWriteArrayList<Pair<Int, PadKeys.KeyChange>>()
        val rumbles = CopyOnWriteArrayList<Pair<Int, Int>>()

        val host = CompanionHost(
            streamServer = bus,
            claimSlot = { key, _ -> slots.connectRemote(key) },
            releaseSlot = { key, _ -> slots.disconnect(key) },
            onInput = { slot, change -> inputs += slot to change },
            timeoutMs = 1_000,
            heartbeatMs = 50,
            handshakeTimeoutMs = 2_000,
        ).start().also { closeables += it }

        assertEquals("Bluetooth (Pixel 9)", host.endpoint)

        val client = CompanionClient(
            streamOpener = { bus.connectClient() },
            code = host.code,
            name = "BT Pad",
            clientId = ByteArray(16) { 7 },
            onRumble = { l, r -> rumbles += l to r },
        ).also { closeables += it }

        assertEquals(1, client.connect()) // Assigned P2 (index 1)
        client.update(PadState(buttons = 0x1000))
        eventually("A button delivered over stream transport") {
            inputs.any { it.first == 1 && it.second.pressed }
        }

        host.sendRumble(1, 32000, 16000)
        eventually("Rumble delivered over stream transport") {
            rumbles.any { it == (32000 to 16000) }
        }
    }

    @Test fun hostControlFallsBackToBluetoothWhenNoLanInterfaceExists() {
        val bus = InMemoryRfcommBus().also { closeables += it }
        val slots = ControllerSlots()
        val control = CompanionHostControl(
            newHost = { error("LAN should not be called when pickNetwork returns null") },
            pickNetwork = { null },
            newBluetoothHost = {
                CompanionHost(
                    streamServer = bus,
                    claimSlot = { key, _ -> slots.connectRemote(key) },
                    releaseSlot = { key, _ -> slots.disconnect(key) },
                    onInput = { _, _ -> },
                )
            },
        ).also { closeables += it }

        control.toggle()
        eventually("Bluetooth fallback host started") {
            control.status is CompanionHostControl.Status.On
        }
        val on = control.status as CompanionHostControl.Status.On
        assertEquals("Bluetooth RFCOMM", on.network)
        assertEquals("Bluetooth (Pixel 9)", on.host.endpoint)
    }
}
