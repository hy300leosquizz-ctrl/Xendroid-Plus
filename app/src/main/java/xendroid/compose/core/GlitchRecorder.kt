package xendroid.compose.core

import java.io.File
import java.util.Locale

/**
 * 6-second diagnostic Glitch Recorder (`Back + Start` chord).
 *
 * Keeps a bounded rolling window (default 6 seconds) of frame rate, frame time, audio concealments,
 * shader compiles, and thermal state. Holding `Back + Start` together for [HOLD_THRESHOLD_MS]
 * freezes the last 6 seconds into an atomic JSON report under `filesDir/glitch_reports/` so stutter
 * or audio-dropout spikes can be diagnosed without running heavy continuous tracing.
 */
class GlitchRecorder(
    private val windowMs: Long = 6_000L,
    private val maxSamples: Int = 64,
) {
    data class Sample(
        val elapsedMs: Long,
        val guestFps: Double,
        val frameMs: Double,
        val presentCount: Long,
        val audioConcealedBlocks: Long,
        val pipelinesCompiled: Long,
        val thermalStatus: Int,
        val zeroFgActive: Boolean,
    )

    private val ring = ArrayDeque<Sample>(maxSamples)

    @Synchronized
    fun record(sample: Sample) {
        val cutoff = sample.elapsedMs - windowMs
        while (ring.isNotEmpty() && ring.first().elapsedMs < cutoff) {
            ring.removeFirst()
        }
        while (ring.size >= maxSamples) {
            ring.removeFirst()
        }
        ring.addLast(sample)
    }

    @Synchronized
    fun snapshot(): List<Sample> = ring.toList()

    /** Pure chord detector for simultaneous `Back + Start` hold (`KC_BACK = 8`, `KC_START = 9`). */
    class ChordDetector(
        private val holdThresholdMs: Long = HOLD_THRESHOLD_MS,
        private val cooldownMs: Long = COOLDOWN_MS,
    ) {
        private var backDown = false
        private var startDown = false
        private var chordStartedAtMs = -1L
        private var firedForCurrentHold = false
        private var lastFiredAtMs = Long.MIN_VALUE

        @Synchronized
        fun onGuestKey(gameKey: Int, pressed: Boolean, nowMs: Long): Boolean {
            when (gameKey) {
                KC_BACK -> backDown = pressed
                KC_START -> startDown = pressed
                else -> return false
            }
            if (backDown && startDown) {
                if (chordStartedAtMs < 0L) {
                    chordStartedAtMs = nowMs
                    firedForCurrentHold = false
                }
                return checkHeld(nowMs)
            } else {
                chordStartedAtMs = -1L
                firedForCurrentHold = false
                return false
            }
        }

        @Synchronized
        fun poll(nowMs: Long): Boolean = if (backDown && startDown) checkHeld(nowMs) else false

        @Synchronized
        fun reset() {
            backDown = false
            startDown = false
            chordStartedAtMs = -1L
            firedForCurrentHold = false
        }

        private fun checkHeld(nowMs: Long): Boolean {
            if (firedForCurrentHold || chordStartedAtMs < 0L) return false
            if (nowMs - chordStartedAtMs < holdThresholdMs) return false
            if (lastFiredAtMs != Long.MIN_VALUE && nowMs - lastFiredAtMs < cooldownMs) return false
            firedForCurrentHold = true
            lastFiredAtMs = nowMs
            return true
        }
    }

    companion object {
        const val KC_BACK = 8
        const val KC_START = 9
        const val HOLD_THRESHOLD_MS = 1_200L
        const val COOLDOWN_MS = 5_000L

        fun formatJson(
            titleId: String?,
            gameName: String?,
            gpuLabel: String?,
            capturedAtEpochMs: Long,
            samples: List<Sample>,
        ): String {
            fun esc(s: String?) = s.orEmpty().replace("\\", "\\\\").replace("\"", "\\\"").replace("\n", " ")
            val minFps = samples.minOfOrNull { it.guestFps } ?: 0.0
            val maxFrameMs = samples.maxOfOrNull { it.frameMs } ?: 0.0
            val xrunDelta = if (samples.size >= 2) {
                (samples.last().audioConcealedBlocks - samples.first().audioConcealedBlocks).coerceAtLeast(0L)
            } else 0L
            return buildString {
                appendLine("{")
                appendLine("  \"format\": \"xendroid-glitch-snapshot\",")
                appendLine("  \"version\": 1,")
                appendLine("  \"capturedAtEpochMs\": $capturedAtEpochMs,")
                appendLine("  \"titleId\": \"${esc(titleId ?: "UNKNOWN")}\",")
                appendLine("  \"gameName\": \"${esc(gameName)}\",")
                appendLine("  \"gpu\": \"${esc(gpuLabel)}\",")
                appendLine("  \"windowSamples\": ${samples.size},")
                appendLine("  \"minFps\": ${String.format(Locale.US, "%.2f", minFps)},")
                appendLine("  \"maxFrameMs\": ${String.format(Locale.US, "%.2f", maxFrameMs)},")
                appendLine("  \"audioConcealedDelta\": $xrunDelta,")
                appendLine("  \"samples\": [")
                samples.forEachIndexed { idx, s ->
                    val comma = if (idx < samples.lastIndex) "," else ""
                    appendLine(
                        "    {\"elapsedMs\":${s.elapsedMs},\"fps\":${String.format(Locale.US, "%.2f", s.guestFps)}," +
                            "\"frameMs\":${String.format(Locale.US, "%.2f", s.frameMs)},\"presents\":${s.presentCount}," +
                            "\"audioConcealed\":${s.audioConcealedBlocks},\"pipelines\":${s.pipelinesCompiled}," +
                            "\"thermal\":${s.thermalStatus},\"zeroFg\":${s.zeroFgActive}}$comma"
                    )
                }
                appendLine("  ]")
                append("}")
            }
        }

        fun writeSnapshot(
            dir: File,
            titleId: String?,
            gameName: String?,
            gpuLabel: String?,
            capturedAtEpochMs: Long,
            samples: List<Sample>,
        ): File {
            dir.mkdirs()
            // Keep at most 16 recent glitch reports so diagnostics storage stays bounded.
            val existing = dir.listFiles { f -> f.isFile && f.name.endsWith(".json") }
                ?.sortedBy { it.lastModified() }.orEmpty()
            if (existing.size >= 16) {
                existing.take(existing.size - 15).forEach { it.delete() }
            }
            val safeTid = titleId?.uppercase(Locale.US)?.takeIf { it.matches(Regex("[0-9A-F]{8}")) } ?: "SESSION"
            val out = File(dir, "glitch_${safeTid}_$capturedAtEpochMs.json")
            out.writeText(formatJson(titleId, gameName, gpuLabel, capturedAtEpochMs, samples), Charsets.UTF_8)
            return out
        }
    }
}
