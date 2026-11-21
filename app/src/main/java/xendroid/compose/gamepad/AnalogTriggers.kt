package xendroid.compose.gamepad

import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.roundToInt

/**
 * #97, #104, #118: a controller's trigger and stick axes as the guest gets them, plus live
 * arbitration between analog trigger axes and digital L2/R2 keys.
 *
 * Handhelds like AYN Odin 2/3 can switch L2/R2 live between Analog, Digital, and Both modes,
 * and controllers without a vendor `.kl` (falling back to `Generic.kl`) expose unsigned 0..1
 * triggers on `AXIS_Z`/`AXIS_RZ` and the signed -1..1 right stick on `AXIS_RX`/`AXIS_RY`.
 */
object AnalogTriggers {
    /** Rest noise under this reads as released. */
    const val DEADZONE = 0.02f
    /** Past this, a trigger mapped to a button presses it (P1's old rule for both triggers). */
    const val BUTTON_THRESHOLD = 0.5f
    const val KC_TRIGGER_L = 14
    const val KC_TRIGGER_R = 15
    private const val KEY_VALUE_UNUSED = -1

    /** Reported [InputDevice.MotionRange] bounds for one axis, kept pure for JVM unit tests. */
    data class AxisRange(val min: Float, val max: Float) {
        val isSignedStick: Boolean get() = min < -0.5f && max > 0.5f
        val isUnsignedTrigger: Boolean get() = min >= -0.05f && max > 0.5f
    }

    private val INSPECTED_AXES = intArrayOf(
        MotionEvent.AXIS_Z,
        MotionEvent.AXIS_RZ,
        MotionEvent.AXIS_RX,
        MotionEvent.AXIS_RY,
        MotionEvent.AXIS_LTRIGGER,
        MotionEvent.AXIS_RTRIGGER,
        MotionEvent.AXIS_BRAKE,
        MotionEvent.AXIS_GAS,
    )

    /**
     * Resolves sticks and triggers from [axis] values using the device's [ranges]:
     * - Standard Android `.kl`: right stick on signed `AXIS_Z`/`AXIS_RZ`, triggers on `LTRIGGER`/`BRAKE` and `RTRIGGER`/`GAS`.
     * - `Generic.kl` / unmapped HID (#118): unsigned `AXIS_Z`/`AXIS_RZ` (0..1) carry LT/RT when standard trigger axes are
     *   absent, while signed `AXIS_RX`/`AXIS_RY` (-1..1) carry the right stick.
     */
    fun resolvePadAxes(ranges: Map<Int, AxisRange>, axis: (Int) -> Float): PadAxes {
        val hasStandardTriggers =
            MotionEvent.AXIS_LTRIGGER in ranges ||
                MotionEvent.AXIS_BRAKE in ranges ||
                MotionEvent.AXIS_RTRIGGER in ranges ||
                MotionEvent.AXIS_GAS in ranges
        val zRange = ranges[MotionEvent.AXIS_Z]
        val rzRange = ranges[MotionEvent.AXIS_RZ]
        val rxSigned = ranges[MotionEvent.AXIS_RX]?.isSignedStick == true
        val rySigned = ranges[MotionEvent.AXIS_RY]?.isSignedStick == true
        val zAsTrigger = !hasStandardTriggers && zRange?.isUnsignedTrigger == true
        val rzAsTrigger = !hasStandardTriggers && rzRange?.isUnsignedTrigger == true

        val rx = when {
            zRange?.isSignedStick == true -> axis(MotionEvent.AXIS_Z)
            rxSigned -> axis(MotionEvent.AXIS_RX)
            ranges.isEmpty() -> axis(MotionEvent.AXIS_Z)
            else -> 0f
        }
        val ry = when {
            rzRange?.isSignedStick == true -> axis(MotionEvent.AXIS_RZ)
            rySigned -> axis(MotionEvent.AXIS_RY)
            ranges.isEmpty() -> axis(MotionEvent.AXIS_RZ)
            else -> 0f
        }
        val lt = maxOf(
            axis(MotionEvent.AXIS_LTRIGGER),
            axis(MotionEvent.AXIS_BRAKE),
            if (zAsTrigger) axis(MotionEvent.AXIS_Z) else 0f,
        )
        val rt = maxOf(
            axis(MotionEvent.AXIS_RTRIGGER),
            axis(MotionEvent.AXIS_GAS),
            if (rzAsTrigger) axis(MotionEvent.AXIS_RZ) else 0f,
        )
        return PadAxes(
            lx = axis(MotionEvent.AXIS_X),
            ly = axis(MotionEvent.AXIS_Y),
            rx = rx,
            ry = ry,
            lt = lt,
            rt = rt,
            hatX = axis(MotionEvent.AXIS_HAT_X),
            hatY = axis(MotionEvent.AXIS_HAT_Y),
        )
    }

    /** Reads normalized [PadAxes] from a joystick [MotionEvent], inspecting its [InputDevice] ranges when available. */
    fun readPadAxes(event: MotionEvent): PadAxes {
        val device = event.device
        val ranges = if (device == null) {
            emptyMap()
        } else {
            buildMap(INSPECTED_AXES.size) {
                for (a in INSPECTED_AXES) {
                    val r = device.getMotionRange(a, event.source) ?: device.getMotionRange(a)
                    if (r != null) put(a, AxisRange(r.min, r.max))
                }
            }
        }
        return resolvePadAxes(ranges) { event.getAxisValue(it) }
    }

    /** A trigger's travel, 0..1, as the guest's trigger byte. */
    fun byteOf(value: Float): Int =
        if (!value.isFinite() || value < DEADZONE) 0 else (value * 255f).roundToInt().coerceIn(0, 255)

    /** One guest key as a motion sample leaves it: [value] is the trigger byte, or unused for a button. */
    data class Output(val key: Int, val pressed: Boolean, val value: Int)

    /**
     * The guest keys one sample of the left and right trigger ([lt], [rt], 0..1) drives, given
     * the guest keys L2 and R2 are mapped to ([ltKey], [rtKey]; null = left unbound, so that
     * trigger drives nothing). Both triggers on one key give it the deeper one.
     */
    fun outputs(lt: Float, rt: Float, ltKey: Int?, rtKey: Int?): List<Output> {
        val travel = linkedMapOf<Int, Float>()
        fun add(key: Int?, value: Float) {
            if (key == null) return
            val v = if (value.isFinite()) value else 0f
            travel[key] = maxOf(travel[key] ?: 0f, v)
        }
        add(ltKey, lt)
        add(rtKey, rt)
        return travel.map { (key, value) ->
            if (key == KC_TRIGGER_L || key == KC_TRIGGER_R) {
                val byte = byteOf(value)
                Output(key, byte > 0, byte)
            } else {
                Output(key, value > BUTTON_THRESHOLD, KEY_VALUE_UNUSED)
            }
        }
    }

    /**
     * Live arbitration between analog trigger axes and `KEYCODE_BUTTON_L2` / `KEYCODE_BUTTON_R2` (#118):
     * - While a trigger's axis is deflected past [DEADZONE], its companion digital L2/R2 key is suppressed
     *   (and if the digital key arrived first in `Both` mode, [onAxis] flags bit 1 / bit 2 so the host forgets
     *   that held key and hands ownership to the analog axis without cutting travel on key-up).
     * - Once the axis returns to rest (`<= DEADZONE`), digital `BUTTON_L2`/`BUTTON_R2` presses are accepted
     *   again immediately so switching a handheld (e.g. AYN Odin 3) from Analog/Both to Digital mode works
     *   live without restarting the game.
     */
    class TriggerKeys {
        private val activeSides = HashSet<Long>()

        /**
         * Updates live axis activity for [deviceId]; returns bitmask (1 = L2 newly active, 2 = R2 newly active)
         * so any digital L2/R2 key pressed moments earlier is handed over to the axis.
         */
        fun onAxis(deviceId: Int, lt: Float, rt: Float): Int {
            val base = deviceId.toLong() shl 1
            var handoff = 0
            if (lt.isFinite() && lt > DEADZONE) {
                if (activeSides.add(base)) handoff = handoff or 1
            } else {
                activeSides.remove(base)
            }
            if (rt.isFinite() && rt > DEADZONE) {
                if (activeSides.add(base or 1L)) handoff = handoff or 2
            } else {
                activeSides.remove(base or 1L)
            }
            return handoff
        }

        /** True while the analog axis corresponding to [keyCode] (`BUTTON_L2`/`BUTTON_R2`) is currently active. */
        fun isAxisActiveForKey(deviceId: Int, keyCode: Int): Boolean {
            val side = when (keyCode) {
                KeyEvent.KEYCODE_BUTTON_L2 -> 0L
                KeyEvent.KEYCODE_BUTTON_R2 -> 1L
                else -> return false
            }
            return ((deviceId.toLong() shl 1) or side) in activeSides
        }

        fun deviceRemoved(deviceId: Int) {
            val base = deviceId.toLong() shl 1
            activeSides.remove(base)
            activeSides.remove(base or 1L)
        }

        fun clear() {
            activeSides.clear()
        }
    }

    /**
     * What the trigger axes last sent per guest key, so only a change goes out. A key the axes
     * never pressed is never released by them: a trigger at rest must not release LT held by a
     * pad's digital ZL key, nor the button a mapping puts a trigger on held by its own key.
     */
    class Sent {
        private val last = HashMap<Int, Output>()

        /** Adopts a guest key already pressed by a companion digital L2/R2 key before its analog axis moved (#118). */
        fun adoptHeldKey(key: Int) {
            if (last[key]?.pressed == true) return
            last[key] = Output(key, true, if (key == KC_TRIGGER_L || key == KC_TRIGGER_R) 255 else KEY_VALUE_UNUSED)
        }

        /** The outputs of one sample that must be sent. */
        fun changes(outputs: List<Output>): List<Output> = outputs.filter { out ->
            val previous = last[out.key]
            if (previous == out || (previous == null && !out.pressed)) return@filter false
            last[out.key] = out
            true
        }

        /** The releases of what the axes hold; then nothing is held. */
        fun releaseAll(): List<Output> {
            val held = last.values.filter { it.pressed }.map { Output(it.key, false, if (it.value < 0) KEY_VALUE_UNUSED else 0) }
            last.clear()
            return held
        }
    }
}
