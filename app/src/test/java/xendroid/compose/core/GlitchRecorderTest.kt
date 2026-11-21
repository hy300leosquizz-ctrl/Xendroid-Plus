package xendroid.compose.core

import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import xendroid.compose.parseDirectGpuPercent

class GlitchRecorderTest {

    @Test fun ringBufferEvictsSamplesOlderThanSixSeconds() {
        val recorder = GlitchRecorder(windowMs = 6_000L, maxSamples = 16)
        recorder.record(GlitchRecorder.Sample(1_000L, 60.0, 16.6, 60, 0, 10, 0, false))
        recorder.record(GlitchRecorder.Sample(4_000L, 58.5, 17.1, 240, 0, 12, 0, false))
        recorder.record(GlitchRecorder.Sample(7_500L, 21.0, 47.6, 400, 3, 19, 2, true))

        val snap = recorder.snapshot()
        assertEquals(2, snap.size)
        assertEquals(4_000L, snap.first().elapsedMs)
        assertEquals(7_500L, snap.last().elapsedMs)

        val dir = Files.createTempDirectory("glitch").toFile()
        val out = GlitchRecorder.writeSnapshot(dir, "4d5307e6", "Halo 3", "Adreno 740", 1700000000000L, snap)
        val json = out.readText()
        assertTrue(json.contains("\"format\": \"xendroid-glitch-snapshot\""))
        assertTrue(json.contains("\"titleId\": \"4d5307e6\""))
        assertTrue(json.contains("\"audioConcealedDelta\": 3"))
        assertTrue(json.contains("\"minFps\": 21.00"))
    }

    @Test fun backPlusStartChordFiresOnceAfterHoldThresholdAndRespectsCooldown() {
        val chord = GlitchRecorder.ChordDetector(holdThresholdMs = 1_200L, cooldownMs = 5_000L)

        assertFalse(chord.onGuestKey(GlitchRecorder.KC_BACK, true, 100L))
        assertFalse(chord.poll(2_000L)) // Start is not down yet
        assertFalse(chord.onGuestKey(GlitchRecorder.KC_START, true, 2_000L))
        assertFalse(chord.poll(3_100L)) // Held 1100 ms < 1200 ms
        assertTrue(chord.poll(3_250L))  // Held 1250 ms -> fires!
        assertFalse(chord.poll(3_500L)) // Does not re-fire during the same hold

        // Releasing and immediately re-holding within 5s cooldown stays suppressed.
        chord.onGuestKey(GlitchRecorder.KC_BACK, false, 3_600L)
        chord.onGuestKey(GlitchRecorder.KC_BACK, true, 4_000L)
        assertFalse(chord.poll(5_500L)) // 5500 - 3250 = 2250 ms < 5000 ms cooldown
        assertTrue(chord.poll(8_300L))  // Cooldown elapsed and still held >= 1200 ms -> fires!
    }

    @Test fun directGpuPercentParserReadsTheKernelCounters() {
        assertEquals(45, parseDirectGpuPercent("45\n"))
        assertEquals(72, parseDirectGpuPercent("72 %"))
        assertEquals(88, parseDirectGpuPercent("88 @ 900000000"))
        assertEquals(100, parseDirectGpuPercent("104.7"))
        assertNull(parseDirectGpuPercent(""))
        assertNull(parseDirectGpuPercent("unavailable"))
    }
}
