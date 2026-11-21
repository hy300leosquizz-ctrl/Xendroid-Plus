package xendroid.compose.core

import android.content.Context
import android.os.Build
import android.os.PerformanceHintManager
import androidx.annotation.RequiresApi
import kotlin.math.roundToLong

/**
 * ADPF only for the native presenter TID that supplied an observed work sample.
 * On Android 13+ (API 33+) the native Vulkan presenter reports actual work duration on every
 * frame via NDK `APerformanceHint_*` (#123); on Android 12 (API 31–32) [Helper] provides a
 * fallback SDK session with deduplicated target updates.
 */
class PresenterPerformanceHints(
    context: Context,
    private val onNativeTargetNs: (Long) -> Unit = {},
) : AutoCloseable {
    private val helper = if (Build.VERSION.SDK_INT >= 31) Helper(context) else null
    private var pushedNativeTargetNs = 0L
    var requested = false
    var status = if (helper == null) "Presenter ADPF · Android 12+ required" else "Presenter ADPF · Off"
        private set

    private fun syncNativeTarget(nextTargetNs: Long) {
        val normalized = if (nextTargetNs > 0L) nextTargetNs else 0L
        if (normalized != pushedNativeTargetNs) {
            pushedNativeTargetNs = normalized
            onNativeTargetNs(normalized)
        }
    }

    fun update(work: LongArray, targetNs: Long, foreground: Boolean) {
        if (Build.VERSION.SDK_INT < 31) {
            syncNativeTarget(0L)
            status = "Presenter ADPF · Android 12+ required"
            return
        }
        val helper = helper
        if (!requested || !foreground) {
            syncNativeTarget(0L)
            helper?.close()
            status = when {
                helper == null -> "Presenter ADPF · unavailable"
                requested -> "Presenter ADPF · requested (suspended in menu/background)"
                else -> "Presenter ADPF · Off"
            }
            return
        }
        val clampedTarget = targetNs.coerceAtLeast(1L)
        syncNativeTarget(clampedTarget)
        val nativeState = if (work.size >= 4) work[3] else 0L
        if (nativeState == 1L) {
            helper?.close()
            status = "Presenter ADPF · active (per-frame native)"
            return
        }
        if (Build.VERSION.SDK_INT >= 33) {
            helper?.close()
            status = if (nativeState < 0L) {
                "Presenter ADPF · unsupported by device"
            } else {
                "Presenter ADPF · waiting for native thread"
            }
            return
        }
        if (helper == null) { status = "Presenter ADPF · unavailable"; return }
        status = runCatching { helper.report(work, clampedTarget) }.getOrElse {
            helper.close(); "Presenter ADPF · unsupported by device"
        }
    }

    override fun close() {
        syncNativeTarget(0L)
        if (Build.VERSION.SDK_INT >= 31) helper?.close()
    }

    @RequiresApi(31)
    private class Helper(context: Context) : AutoCloseable {
        private val manager = context.getSystemService(PerformanceHintManager::class.java)
        private var session: PerformanceHintManager.Session? = null
        private var tid = 0
        private var sequence = -1L
        private var lastTargetNs = 0L

        fun report(work: LongArray, target: Long): String {
            if (work.size < 3 || work[0] <= 0) return "Presenter ADPF · waiting for native thread"
            val nextTid = work[0].toInt()
            val clamped = target.coerceAtLeast(1L)
            if (nextTid != tid || session == null) {
                close()
                session = manager?.createHintSession(intArrayOf(nextTid), clamped)
                tid = nextTid
                lastTargetNs = if (session != null) clamped else 0L
            }
            val active = session ?: return "Presenter ADPF · unavailable"
            if (shouldUpdateTarget(lastTargetNs, clamped)) {
                active.updateTargetWorkDuration(clamped)
                lastTargetNs = clamped
            }
            if (work[2] != sequence && work[1] > 0) {
                active.reportActualWorkDuration(work[1])
                sequence = work[2]
            }
            return "Presenter ADPF · active (native presentation only)"
        }

        override fun close() {
            session?.close()
            session = null
            tid = 0
            sequence = -1L
            lastTargetNs = 0L
        }
    }

    companion object {
        /**
         * Target frame work duration in nanoseconds for [fpsLimit] (0 = unlimited, paced by [displayHz]).
         * Avoids clamping Unlimited (0) to 30 FPS (33.3 ms) on 60/120 Hz panels (#123).
         */
        fun targetDurationNs(fpsLimit: Int, displayHz: Float): Long {
            val effectiveFps = if (fpsLimit > 0) {
                fpsLimit.toDouble()
            } else if (displayHz.isFinite() && displayHz > 0f) {
                displayHz.toDouble()
            } else {
                60.0
            }
            val clampedFps = effectiveFps.coerceIn(15.0, 240.0)
            return (1_000_000_000.0 / clampedFps).roundToLong().coerceAtLeast(1L)
        }

        /** True only when [nextTargetNs] is positive and differs from [lastTargetNs] (#123). */
        fun shouldUpdateTarget(lastTargetNs: Long, nextTargetNs: Long): Boolean =
            nextTargetNs > 0L && nextTargetNs != lastTargetNs
    }
}
