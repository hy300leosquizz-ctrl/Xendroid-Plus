package xendroid.compose.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PresenterPerformanceHintsTest {
    @Test fun targetDurationFollowsExplicitFpsLimitAndDisplayHzWhenUnlimited() {
        assertEquals(33_333_333L, PresenterPerformanceHints.targetDurationNs(30, 120f))
        assertEquals(25_000_000L, PresenterPerformanceHints.targetDurationNs(40, 120f))
        assertEquals(16_666_667L, PresenterPerformanceHints.targetDurationNs(60, 120f))
        // Unlimited (0) follows the panel refresh rate instead of clamping to 30 FPS (#123).
        assertEquals(16_666_667L, PresenterPerformanceHints.targetDurationNs(0, 60f))
        assertEquals(8_333_333L, PresenterPerformanceHints.targetDurationNs(0, 120f))
        assertEquals(16_666_667L, PresenterPerformanceHints.targetDurationNs(0, Float.NaN))
        assertEquals(16_666_667L, PresenterPerformanceHints.targetDurationNs(0, 0f))
    }

    @Test fun targetUpdatesOnlyWhenChangedAndPositive() {
        assertFalse(PresenterPerformanceHints.shouldUpdateTarget(16_666_667L, 16_666_667L))
        assertTrue(PresenterPerformanceHints.shouldUpdateTarget(16_666_667L, 25_000_000L))
        assertTrue(PresenterPerformanceHints.shouldUpdateTarget(0L, 16_666_667L))
        assertFalse(PresenterPerformanceHints.shouldUpdateTarget(16_666_667L, 0L))
    }
}
