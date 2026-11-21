package xendroid.compose.companion

import java.net.ConnectException
import java.net.NoRouteToHostException
import java.net.SocketTimeoutException
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/** The game's address as its menu shows it, typed on the phone that controls ("192.168.1.20:41234"). */
data class CompanionTarget(val host: String, val port: Int) {
    override fun toString() = "$host:$port"

    companion object {
        private val form = Regex("""(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3}):(\d{1,5})""")
        private val code = Regex("[0-9]{6}")

        /** An IPv4 literal and a port; null for anything else (names are never looked up). */
        fun parse(text: String): CompanionTarget? {
            val match = form.matchEntire(text.trim()) ?: return null
            val octets = match.groupValues.subList(1, 5).map(String::toInt)
            val port = match.groupValues[5].toInt()
            if (octets.any { it > 255 } || port !in 1..65535) return null
            return CompanionTarget(octets.joinToString("."), port)
        }

        fun isCode(text: String): Boolean = code.matches(text.trim())
    }
}

/**
 * The phone-as-controller side (I06) behind its screen: one connection at a time, the
 * on-screen pad's key events folded into full states ([PadKeys.apply]), the game's
 * rumble passed on, and a state the UI can show. [connect] blocks: call it off Android's
 * main thread; everything else returns at once.
 */
class CompanionPadLink(
    private val newClient: (target: CompanionTarget, code: String, name: String,
                            onRumble: (Int, Int) -> Unit, onClosed: (String) -> Unit) -> CompanionClient,
    /** The game's rumble for this phone's slot: left motor shl 16 or right motor; 0 stops. */
    private val onRumble: (motors: Long) -> Unit = {},
    private val newBluetoothClient: ((btMac: String, code: String, name: String,
                                      onRumble: (Int, Int) -> Unit, onClosed: (String) -> Unit) -> CompanionClient)? = null,
) {
    sealed interface State {
        /** Not connected; [why] says why the last attempt or connection ended. */
        data class Idle(val why: Why? = null) : State
        data object Connecting : State
        /** [slot] 1..3 = P2..P4; [latencyMs] the round trip the game measured. */
        data class Playing(val slot: Int, val latencyMs: Int? = null) : State
    }

    /**
     * Why the phone is not connected, for its screen to say in the shown language (U02).
     * [target]: the address tried; [code]: the game's rejection code; [detail]: the system's or
     * the connection's own words, shown as they are.
     */
    data class Why(val kind: Kind, val target: String = "", val code: Int = 0, val detail: String = "") {
        enum class Kind { BAD_ADDRESS, BAD_CODE, REJECTED, UNREACHABLE, NOT_XENDROID, FAILED, DISCONNECTED }
    }

    private val mutableState = MutableStateFlow<State>(State.Idle())
    val state: StateFlow<State> = mutableState

    private var client: CompanionClient? = null
    private var pad = PadState.RELEASED

    fun connect(address: String, code: String, name: String) {
        val target = CompanionTarget.parse(address)
        val btMac = if (target == null && newBluetoothClient != null) CompanionBluetooth.parseMac(address) else null
        if (target == null && btMac == null) return idle(Why(Why.Kind.BAD_ADDRESS))
        if (!CompanionTarget.isCode(code)) return idle(Why(Why.Kind.BAD_CODE))
        val targetLabel = target?.toString() ?: "bt://$btMac"
        val created = synchronized(this) {
            if (client != null) return
            var self: CompanionClient? = null
            val cleanCode = code.trim()
            val cleanName = name.trim().ifEmpty { "Phone" }
            val rumbleCb: (Int, Int) -> Unit = { left, right -> onRumble((left.toLong() shl 16) or right.toLong()) }
            val closedCb: (String) -> Unit = { reason -> self?.let { ended(it, reason) } }
            val made = if (target != null) {
                newClient(target, cleanCode, cleanName, rumbleCb, closedCb)
            } else {
                newBluetoothClient!!.invoke(btMac!!, cleanCode, cleanName, rumbleCb, closedCb)
            }
            self = made
            client = made
            pad = PadState.RELEASED
            mutableState.value = State.Connecting
            made
        }
        try {
            val slot = created.connect()
            synchronized(this) { if (client === created) mutableState.value = State.Playing(slot) }
        } catch (e: Exception) {
            synchronized(this) {
                if (client !== created) return  // the user left while connecting
                client = null
                mutableState.value = State.Idle(describe(e, targetLabel))
            }
        }
    }

    /** One event of the on-screen pad (GamepadEmitter's codes and values). */
    fun key(key: Int, pressed: Boolean, value: Int) {
        synchronized(this) {
            pad = PadKeys.apply(pad, key, pressed, value)
            client?.update(pad)
        }
    }

    /** Picks up the latest latency; the screen calls it every second. */
    fun refresh() {
        synchronized(this) {
            val playing = mutableState.value as? State.Playing ?: return
            val latency = client?.latencyMs
            if (latency != playing.latencyMs) mutableState.value = playing.copy(latencyMs = latency)
        }
    }

    /** Leaves the game (or stops connecting); the game releases this phone's buttons at once. */
    fun leave() {
        val leaving = synchronized(this) {
            val current = client ?: return
            client = null
            pad = PadState.RELEASED
            mutableState.value = State.Idle()
            current
        }
        leaving.close()
    }

    private fun ended(which: CompanionClient, reason: String) {
        synchronized(this) {
            if (client !== which) return
            client = null
            pad = PadState.RELEASED
            mutableState.value = State.Idle(Why(Why.Kind.DISCONNECTED, detail = reason))
        }
    }

    private fun idle(why: Why) {
        synchronized(this) { if (client == null) mutableState.value = State.Idle(why) }
    }

    private fun describe(e: Exception, target: String): Why = when (e) {
        is CompanionRejected -> Why(Why.Kind.REJECTED, target, code = e.reason)
        is ConnectException, is SocketTimeoutException, is NoRouteToHostException -> Why(Why.Kind.UNREACHABLE, target)
        is CompanionProtocolException -> Why(Why.Kind.NOT_XENDROID, target, detail = e.message.orEmpty())
        else -> Why(Why.Kind.FAILED, target, detail = e.message ?: e.javaClass.simpleName)
    }
}
