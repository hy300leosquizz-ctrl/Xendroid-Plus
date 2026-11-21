package xendroid.compose.companion

import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.Semaphore
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference
import kotlin.concurrent.thread

class CompanionRejected(val reason: Int) : IOException(
    when (reason) {
        CompanionProtocol.REJECT_PROOF -> "Wrong code"
        CompanionProtocol.REJECT_FULL -> "All player slots are taken"
        CompanionProtocol.REJECT_VERSION -> "The game runs another XenDroid version"
        CompanionProtocol.REJECT_CLOSED -> "Pairing is closed on the game (turn phone controllers off and on there for a new code)"
        else -> "Refused ($reason)"
    })

/**
 * The phone-as-controller side (I06) over LAN TCP or Bluetooth RFCOMM: joins a host with the
 * 6-digit code it shows, then streams full pad states and receives guest rumble.
 */
class CompanionClient private constructor(
    private val openStream: (timeoutMs: Int) -> CompanionHost.StreamConn,
    private val abortPendingConnect: () -> Unit,
    private val code: String,
    private val name: String,
    private val clientId: ByteArray,
    private val onRumble: (left: Int, right: Int) -> Unit,
    private val onClosed: (reason: String) -> Unit,
) : Closeable {

    constructor(
        host: String,
        port: Int,
        code: String,
        name: String,
        clientId: ByteArray,
        onRumble: (left: Int, right: Int) -> Unit = { _, _ -> },
        onClosed: (reason: String) -> Unit = {},
    ) : this(
        tcpSocket = Socket(),
        host = host,
        port = port,
        code = code,
        name = name,
        clientId = clientId,
        onRumble = onRumble,
        onClosed = onClosed,
    )

    private constructor(
        tcpSocket: Socket,
        host: String,
        port: Int,
        code: String,
        name: String,
        clientId: ByteArray,
        onRumble: (left: Int, right: Int) -> Unit,
        onClosed: (reason: String) -> Unit,
    ) : this(
        openStream = { timeoutMs ->
            tcpSocket.connect(InetSocketAddress(host, port), timeoutMs)
            tcpSocket.tcpNoDelay = true
            tcpSocket.soTimeout = timeoutMs
            object : CompanionHost.StreamConn {
                override val inputStream: InputStream get() = tcpSocket.getInputStream()
                override val outputStream: OutputStream get() = tcpSocket.getOutputStream()
                override fun clearTimeout() { tcpSocket.soTimeout = 0 }
                override fun close() = tcpSocket.close()
            }
        },
        abortPendingConnect = { try { tcpSocket.close() } catch (_: IOException) {} },
        code = code,
        name = name,
        clientId = clientId,
        onRumble = onRumble,
        onClosed = onClosed,
    )

    /** Secondary constructor for non-TCP stream transports such as Bluetooth RFCOMM. */
    constructor(
        streamOpener: (timeoutMs: Int) -> CompanionHost.StreamConn,
        code: String,
        name: String,
        clientId: ByteArray,
        onRumble: (left: Int, right: Int) -> Unit = { _, _ -> },
        onClosed: (reason: String) -> Unit = {},
    ) : this(
        openStream = streamOpener,
        abortPendingConnect = {},
        code = code,
        name = name,
        clientId = clientId,
        onRumble = onRumble,
        onClosed = onClosed,
    )

    @Volatile private var activeConn: CompanionHost.StreamConn? = null
    private val writeLock = Any()
    private val open = AtomicBoolean(false)
    private val latest = AtomicReference(PadState.RELEASED)
    private val wake = Semaphore(0)

    /** Connects and pairs; returns the player slot (1..3 = P2..P4). */
    fun connect(timeoutMs: Int = 3_000): Int {
        val (conn, input, slot) = try {
            val c = openStream(timeoutMs)
            activeConn = c
            val input = c.inputStream.buffered()
            val challenge = CompanionCodec.read(input) as? CompanionMessage.Challenge
                ?: throw CompanionProtocolException("Not a XenDroid game")
            if (challenge.version != CompanionProtocol.VERSION) throw CompanionRejected(CompanionProtocol.REJECT_VERSION)
            write(CompanionMessage.Hello(CompanionProtocol.VERSION, clientId, name,
                CompanionProtocol.proof(code, challenge.nonce, clientId)))
            when (val reply = CompanionCodec.read(input)) {
                is CompanionMessage.Welcome -> Triple(c, input, reply.slot)
                is CompanionMessage.Reject -> throw CompanionRejected(reply.reason)
                else -> throw CompanionProtocolException("Unexpected reply")
            }
        } catch (e: Exception) {
            runCatching { activeConn?.close() }
            abortPendingConnect()
            throw e
        }
        conn.clearTimeout()
        open.set(true)
        thread(name = "companion-client-read", isDaemon = true) { readLoop(input) }
        thread(name = "companion-client-send", isDaemon = true) { sendLoop() }
        return slot
    }

    fun update(pad: PadState) {
        latest.set(pad)
        wake.release()
    }

    val isOpen: Boolean get() = open.get()

    @Volatile var latencyMs: Int? = null
        private set

    private fun write(message: CompanionMessage) {
        val out: OutputStream = activeConn?.outputStream ?: throw IOException("Socket not connected")
        synchronized(writeLock) { CompanionCodec.write(out, message) }
    }

    private fun sendLoop() {
        var sent: PadState? = null
        var seq = 0L
        try {
            while (true) {
                wake.acquire()
                wake.drainPermits()
                if (!open.get()) break
                val pad = latest.get()
                if (pad != sent) {
                    write(CompanionMessage.State(++seq, pad))
                    sent = pad
                }
            }
            runCatching { write(CompanionMessage.Bye) }
            runCatching { activeConn?.close() }
        } catch (e: Exception) {
            finish(e.message ?: "connection lost")
        }
    }

    private fun readLoop(input: InputStream) {
        val reason = try {
            while (true) {
                when (val message = CompanionCodec.read(input)) {
                    is CompanionMessage.Ping -> {
                        write(CompanionMessage.Pong(message.time))
                        if (message.rttMs >= 0) latencyMs = message.rttMs
                    }
                    is CompanionMessage.Rumble -> onRumble(message.left, message.right)
                    CompanionMessage.Bye -> break
                    else -> {}
                }
            }
            "the game closed the connection"
        } catch (e: Exception) {
            e.message ?: "connection lost"
        }
        finish(reason)
    }

    private fun finish(reason: String) {
        if (!open.getAndSet(false)) return
        wake.release()
        runCatching { activeConn?.close() }
        onRumble(0, 0)
        onClosed(reason)
    }

    override fun close() {
        if (open.getAndSet(false)) {
            wake.release()
            onRumble(0, 0)
            onClosed("you left")
        } else {
            runCatching { activeConn?.close() }
            abortPendingConnect()
        }
    }
}
