package xendroid.compose.companion

import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import kotlin.concurrent.thread

/**
 * The game side of the companion protocol (I07/I08) over LAN TCP or Bluetooth RFCOMM fallback.
 * A phone that proves the 6-digit code shown on the host gets a free player slot P2..P4 (never P1)
 * and its full pad states become that slot's input.
 */
class CompanionHost private constructor(
    private val streamServer: StreamServer,
    val address: InetAddress,
    val port: Int,
    private val claimSlot: (clientKey: String, name: String) -> Int?,
    private val releaseSlot: (clientKey: String, slot: Int) -> Unit,
    private val onInput: (slot: Int, change: PadKeys.KeyChange) -> Unit,
    private val onEvent: (String) -> Unit,
    private val clock: () -> Long,
    private val timeoutMs: Long,
    private val heartbeatMs: Long,
    private val handshakeTimeoutMs: Int,
) : Closeable {

    interface StreamConn : Closeable {
        val inputStream: InputStream
        val outputStream: OutputStream
        fun configureHandshakeTimeout(ms: Int) {}
        fun clearTimeout() {}
    }

    interface StreamServer : Closeable {
        val endpointLabel: String
        fun accept(): StreamConn
    }

    constructor(
        bindAddress: InetAddress,
        claimSlot: (clientKey: String, name: String) -> Int?,
        releaseSlot: (clientKey: String, slot: Int) -> Unit,
        onInput: (slot: Int, change: PadKeys.KeyChange) -> Unit,
        onEvent: (String) -> Unit = {},
        clock: () -> Long = System::currentTimeMillis,
        timeoutMs: Long = 1_000,
        heartbeatMs: Long = 250,
        handshakeTimeoutMs: Int = 4_000,
        port: Int = 0,
    ) : this(
        tcpServer = ServerSocket().apply {
            reuseAddress = true
            bind(InetSocketAddress(bindAddress, port))
        },
        claimSlot = claimSlot,
        releaseSlot = releaseSlot,
        onInput = onInput,
        onEvent = onEvent,
        clock = clock,
        timeoutMs = timeoutMs,
        heartbeatMs = heartbeatMs,
        handshakeTimeoutMs = handshakeTimeoutMs,
    )

    private constructor(
        tcpServer: ServerSocket,
        claimSlot: (clientKey: String, name: String) -> Int?,
        releaseSlot: (clientKey: String, slot: Int) -> Unit,
        onInput: (slot: Int, change: PadKeys.KeyChange) -> Unit,
        onEvent: (String) -> Unit,
        clock: () -> Long,
        timeoutMs: Long,
        heartbeatMs: Long,
        handshakeTimeoutMs: Int,
    ) : this(
        streamServer = object : StreamServer {
            override val endpointLabel: String = "${tcpServer.inetAddress.hostAddress}:${tcpServer.localPort}"
            override fun accept(): StreamConn {
                val sock = tcpServer.accept()
                return object : StreamConn {
                    override val inputStream: InputStream get() = sock.getInputStream()
                    override val outputStream: OutputStream get() = sock.getOutputStream()
                    override fun configureHandshakeTimeout(ms: Int) {
                        sock.tcpNoDelay = true
                        sock.soTimeout = ms
                    }
                    override fun clearTimeout() {
                        sock.soTimeout = 0
                    }
                    override fun close() {
                        sock.close()
                    }
                }
            }
            override fun close() = tcpServer.close()
        },
        address = tcpServer.inetAddress,
        port = tcpServer.localPort,
        claimSlot = claimSlot,
        releaseSlot = releaseSlot,
        onInput = onInput,
        onEvent = onEvent,
        clock = clock,
        timeoutMs = timeoutMs,
        heartbeatMs = heartbeatMs,
        handshakeTimeoutMs = handshakeTimeoutMs,
    )

    /** Secondary constructor for non-TCP stream transports such as Bluetooth RFCOMM. */
    constructor(
        streamServer: StreamServer,
        claimSlot: (clientKey: String, name: String) -> Int?,
        releaseSlot: (clientKey: String, slot: Int) -> Unit,
        onInput: (slot: Int, change: PadKeys.KeyChange) -> Unit,
        onEvent: (String) -> Unit = {},
        clock: () -> Long = System::currentTimeMillis,
        timeoutMs: Long = 1_500,
        heartbeatMs: Long = 250,
        handshakeTimeoutMs: Int = 5_000,
    ) : this(
        streamServer = streamServer,
        address = InetAddress.getLoopbackAddress(),
        port = 0,
        claimSlot = claimSlot,
        releaseSlot = releaseSlot,
        onInput = onInput,
        onEvent = onEvent,
        clock = clock,
        timeoutMs = timeoutMs,
        heartbeatMs = heartbeatMs,
        handshakeTimeoutMs = handshakeTimeoutMs,
    )

    /** Shown to the user; a new one every time companion mode is turned on. */
    val code: String = CompanionProtocol.newCode()
    val endpoint: String get() = streamServer.endpointLabel

    private val running = AtomicBoolean(true)
    private val clients = ConcurrentHashMap<String, Client>()
    private val joinLock = Any()
    private val pairLock = Any()
    private var wrongCodes = 0
    private val handshakes = AtomicInteger(0)
    private val rumbleSender = Executors.newSingleThreadExecutor { r -> Thread(r, "companion-rumble").apply { isDaemon = true } }
    @Volatile private var inputHeld = false
    private val holdLock = Any()

    data class ClientInfo(val slot: Int, val name: String, val latencyMs: Long?, val connection: Long)

    private inner class Client(val key: String, val name: String, val slot: Int, val conn: StreamConn) {
        val connection = connections.incrementAndGet()
        @Volatile var lastHeard: Long = clock()
        @Volatile var latencyMs: Long? = null
        var lastSeq = -1L
        private var latest = PadState.RELEASED
        private var applied = PadState.RELEASED
        private val writeLock = Any()
        private val closed = AtomicBoolean(false)

        fun send(message: CompanionMessage) {
            synchronized(writeLock) { CompanionCodec.write(conn.outputStream, message) }
        }

        private fun apply(next: PadState) {
            PadKeys.diff(applied, next).forEach { onInput(slot, it) }
            applied = next
        }

        fun update(next: PadState) {
            synchronized(this) {
                if (closed.get()) return
                latest = next
                if (!inputHeld) apply(next)
            }
        }

        fun hold(held: Boolean) {
            synchronized(this) {
                if (closed.get()) return
                apply(if (held) PadState.RELEASED else latest)
            }
        }

        fun close(reason: String) {
            if (!closed.compareAndSet(false, true)) return
            synchronized(this) { apply(PadState.RELEASED) }
            clients.remove(key, this)
            releaseSlot(key, slot)
            runCatching { send(CompanionMessage.Bye) }
            runCatching { conn.close() }
            onEvent("phone controller P${slot + 1} left: $reason")
        }
    }

    val connected: List<ClientInfo>
        get() = clients.values.map { ClientInfo(it.slot, it.name, it.latencyMs, it.connection) }.sortedBy { it.slot }

    fun holdInput(held: Boolean) {
        synchronized(holdLock) {
            if (inputHeld == held) return
            inputHeld = held
            clients.values.forEach { it.hold(held) }
        }
    }

    val locked: Boolean get() = synchronized(pairLock) { wrongCodes >= MAX_WRONG_CODES }

    fun start(): CompanionHost {
        thread(name = "companion-accept", isDaemon = true) {
            while (running.get()) {
                val conn = try { streamServer.accept() } catch (e: IOException) { break }
                if (handshakes.incrementAndGet() > MAX_HANDSHAKES) {
                    handshakes.decrementAndGet()
                    runCatching { conn.close() }
                    continue
                }
                thread(name = "companion-client", isDaemon = true) { serve(conn) }
            }
        }
        thread(name = "companion-heartbeat", isDaemon = true) {
            while (running.get()) {
                try { Thread.sleep(heartbeatMs) } catch (e: InterruptedException) { break }
                val now = clock()
                for (client in clients.values) {
                    val silent = now - client.lastHeard
                    val rtt = client.latencyMs?.coerceAtMost(Short.MAX_VALUE.toLong())?.toInt() ?: -1
                    if (silent > timeoutMs) client.close("no data for $silent ms")
                    else runCatching { client.send(CompanionMessage.Ping(now, rtt)) }.onFailure { client.close("send failed") }
                }
            }
        }
        return this
    }

    fun sendRumble(slot: Int, left: Int, right: Int) {
        val client = clients.values.firstOrNull { it.slot == slot } ?: return
        runCatching {
            rumbleSender.execute { runCatching { client.send(CompanionMessage.Rumble(left, right)) } }
        }
    }

    private fun serve(conn: StreamConn) {
        var handshaking = true
        var client: Client? = null
        fun handshakeDone() { if (handshaking) { handshaking = false; handshakes.decrementAndGet() } }
        try {
            conn.configureHandshakeTimeout(handshakeTimeoutMs)
            val input = conn.inputStream.buffered()
            val nonce = CompanionProtocol.newNonce()
            CompanionCodec.write(conn.outputStream, CompanionMessage.Challenge(CompanionProtocol.VERSION, nonce))
            val hello = CompanionCodec.read(input) as? CompanionMessage.Hello
                ?: throw CompanionProtocolException("Expected HELLO")
            fun reject(reason: Int, why: String) {
                runCatching { CompanionCodec.write(conn.outputStream, CompanionMessage.Reject(reason)) }
                onEvent("phone controller refused: $why")
                conn.close()
            }
            if (hello.version != CompanionProtocol.VERSION) return reject(CompanionProtocol.REJECT_VERSION, "protocol ${hello.version}")
            val verdict = synchronized(pairLock) {
                when {
                    wrongCodes >= MAX_WRONG_CODES -> CompanionProtocol.REJECT_CLOSED
                    !CompanionProtocol.proofMatches(code, nonce, hello.clientId, hello.proof) -> {
                        wrongCodes++
                        CompanionProtocol.REJECT_PROOF
                    }
                    else -> 0
                }
            }
            when (verdict) {
                CompanionProtocol.REJECT_CLOSED -> return reject(verdict, "pairing locked after $MAX_WRONG_CODES wrong codes")
                CompanionProtocol.REJECT_PROOF -> return reject(verdict, "wrong code")
            }
            val key = KEY_PREFIX + UUID.nameUUIDFromBytes(hello.clientId)
            val name = hello.name.filter { it >= ' ' }.trim().take(32).ifBlank { "Phone" }
            val joined = synchronized(joinLock) {
                if (!running.get()) return reject(CompanionProtocol.REJECT_CLOSED, "companion mode off")
                clients[key]?.close("reconnected")
                val slot = claimSlot(key, name) ?: return reject(CompanionProtocol.REJECT_FULL, "no free player slot")
                Client(key, name, slot, conn).also { clients[key] = it }
            }
            client = joined
            handshakeDone()
            conn.clearTimeout()
            joined.send(CompanionMessage.Welcome(joined.slot))
            onEvent("phone controller joined as P${joined.slot + 1}")
            while (running.get()) {
                when (val message = CompanionCodec.read(input)) {
                    is CompanionMessage.State -> {
                        joined.lastHeard = clock()
                        if (message.seq > joined.lastSeq) {
                            joined.lastSeq = message.seq
                            joined.update(message.pad)
                        }
                    }
                    is CompanionMessage.Pong -> {
                        val now = clock()
                        joined.lastHeard = now
                        joined.latencyMs = (now - message.time).takeIf { it >= 0 }
                    }
                    CompanionMessage.Bye -> { joined.close("phone left"); return }
                    else -> throw CompanionProtocolException("Unexpected frame from a phone")
                }
            }
        } catch (e: Exception) {
            val current = client
            if (current != null) current.close(e.message ?: e.javaClass.simpleName) else runCatching { conn.close() }
        } finally {
            handshakeDone()
        }
    }

    override fun close() {
        synchronized(joinLock) {
            if (!running.compareAndSet(true, false)) return
            runCatching { streamServer.close() }
            clients.values.toList().forEach { it.close("companion mode off") }
        }
        rumbleSender.shutdown()
    }

    companion object {
        const val MAX_WRONG_CODES = 10
        const val MAX_HANDSHAKES = 4
        const val KEY_PREFIX = "companion:"
        private val connections = AtomicLong()
    }
}
