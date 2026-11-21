package xendroid.compose.gamepad

import android.view.KeyEvent
import android.view.MotionEvent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import xendroid.compose.gamepad.AnalogTriggers.AxisRange
import xendroid.compose.gamepad.AnalogTriggers.Output

class AnalogTriggersTest {
    @Test fun travelBecomesTheGuestsTriggerByte() {
        assertEquals(0, AnalogTriggers.byteOf(0f))
        assertEquals(0, AnalogTriggers.byteOf(0.015f))        // rest noise
        assertEquals(0, AnalogTriggers.byteOf(Float.NaN))
        assertEquals(0, AnalogTriggers.byteOf(-0.5f))
        assertEquals(128, AnalogTriggers.byteOf(0.5f))
        assertEquals(255, AnalogTriggers.byteOf(1f))
        assertEquals(255, AnalogTriggers.byteOf(1.2f))         // a pad reporting past its range
    }

    @Test fun triggersOnLtAndRtKeepTheirTravelAndAnUnboundOneDrivesNothing() {
        assertEquals(listOf(Output(14, true, 64), Output(15, false, 0)),
            AnalogTriggers.outputs(0.25f, 0f, 14, 15))
        assertEquals(listOf(Output(15, false, 0)), AnalogTriggers.outputs(0.9f, 0f, null, 15))
    }

    @Test fun theAxesSendChangesAndNeverReleaseWhatTheyDidNotPress() {
        val sent = AnalogTriggers.Sent()
        assertEquals(emptyList<Output>(), sent.changes(AnalogTriggers.outputs(0f, 0f, 14, 4)))  // at rest
        assertEquals(listOf(Output(14, true, 128)), sent.changes(AnalogTriggers.outputs(0.5f, 0f, 14, 4)))
        assertEquals(emptyList<Output>(), sent.changes(AnalogTriggers.outputs(0.5f, 0f, 14, 4)))  // no change
        assertEquals(listOf(Output(14, false, 0)), sent.changes(AnalogTriggers.outputs(0f, 0f, 14, 4)))
        assertEquals(listOf(Output(4, true, -1)), sent.changes(AnalogTriggers.outputs(0f, 0.9f, 14, 4)))
        assertEquals(listOf(Output(4, false, -1)), sent.releaseAll())
        assertEquals(emptyList<Output>(), sent.releaseAll())
    }

    @Test fun aTriggerOnAButtonPressesItPastHalfWay() {
        assertEquals(listOf(Output(4, false, -1), Output(15, true, 255)),
            AnalogTriggers.outputs(0.5f, 1f, 4, 15))
        assertEquals(listOf(Output(4, true, -1), Output(15, false, 0)),
            AnalogTriggers.outputs(0.51f, 0f, 4, 15))
    }

    @Test fun bothTriggersOnOneKeyGiveTheDeeperOne() {
        assertEquals(listOf(Output(14, true, 204)), AnalogTriggers.outputs(0.3f, 0.8f, 14, 14))
    }

    @Test fun resolvePadAxesSupportsStandardAndGenericHidLayouts() {
        // Standard Android .kl: signed Z/RZ right stick + LTRIGGER/RTRIGGER triggers.
        val standardRanges = mapOf(
            MotionEvent.AXIS_Z to AxisRange(-1f, 1f),
            MotionEvent.AXIS_RZ to AxisRange(-1f, 1f),
            MotionEvent.AXIS_LTRIGGER to AxisRange(0f, 1f),
            MotionEvent.AXIS_RTRIGGER to AxisRange(0f, 1f),
        )
        val standardValues = mapOf(
            MotionEvent.AXIS_X to -0.25f,
            MotionEvent.AXIS_Y to 0.5f,
            MotionEvent.AXIS_Z to 0.75f,
            MotionEvent.AXIS_RZ to -0.4f,
            MotionEvent.AXIS_LTRIGGER to 0.6f,
            MotionEvent.AXIS_RTRIGGER to 0.9f,
        )
        assertEquals(
            PadAxes(lx = -0.25f, ly = 0.5f, rx = 0.75f, ry = -0.4f, lt = 0.6f, rt = 0.9f),
            AnalogTriggers.resolvePadAxes(standardRanges) { standardValues[it] ?: 0f },
        )

        // Generic.kl / unmapped handheld HID (#118): unsigned 0..1 Z/RZ triggers + signed -1..1 RX/RY right stick.
        val genericHidRanges = mapOf(
            MotionEvent.AXIS_Z to AxisRange(0f, 1f),
            MotionEvent.AXIS_RZ to AxisRange(0f, 1f),
            MotionEvent.AXIS_RX to AxisRange(-1f, 1f),
            MotionEvent.AXIS_RY to AxisRange(-1f, 1f),
        )
        val genericHidValues = mapOf(
            MotionEvent.AXIS_Z to 0.8f,
            MotionEvent.AXIS_RZ to 0.35f,
            MotionEvent.AXIS_RX to -0.6f,
            MotionEvent.AXIS_RY to 0.2f,
        )
        assertEquals(
            PadAxes(rx = -0.6f, ry = 0.2f, lt = 0.8f, rt = 0.35f),
            AnalogTriggers.resolvePadAxes(genericHidRanges) { genericHidValues[it] ?: 0f },
        )
    }

    @Test fun triggerKeysArbitratesLiveBetweenAnalogBothAndDigitalModes() {
        val keys = AnalogTriggers.TriggerKeys()
        // Initially at rest: digital BUTTON_L2 / BUTTON_R2 are accepted.
        assertFalse(keys.isAxisActiveForKey(7, KeyEvent.KEYCODE_BUTTON_L2))
        assertFalse(keys.isAxisActiveForKey(7, KeyEvent.KEYCODE_BUTTON_R2))

        // Analog / Both pull on LT: flags bit 1 once for handoff, suppresses BUTTON_L2 while active.
        assertEquals(1, keys.onAxis(7, 0.4f, 0f))
        assertEquals(0, keys.onAxis(7, 0.7f, 0f))
        assertTrue(keys.isAxisActiveForKey(7, KeyEvent.KEYCODE_BUTTON_L2))
        assertFalse(keys.isAxisActiveForKey(7, KeyEvent.KEYCODE_BUTTON_R2))

        // Trigger returns to rest and user switches handheld to Digital mode: BUTTON_L2 works immediately!
        assertEquals(0, keys.onAxis(7, 0f, 0f))
        assertFalse(keys.isAxisActiveForKey(7, KeyEvent.KEYCODE_BUTTON_L2))

        // Disconnecting or clearing mid-pull resets live state cleanly.
        assertEquals(2, keys.onAxis(7, 0f, 0.9f))
        assertTrue(keys.isAxisActiveForKey(7, KeyEvent.KEYCODE_BUTTON_R2))
        keys.deviceRemoved(7)
        assertFalse(keys.isAxisActiveForKey(7, KeyEvent.KEYCODE_BUTTON_R2))

        assertEquals(1, keys.onAxis(9, 0.6f, 0f))
        assertTrue(keys.isAxisActiveForKey(9, KeyEvent.KEYCODE_BUTTON_L2))
        keys.clear()
        assertFalse(keys.isAxisActiveForKey(9, KeyEvent.KEYCODE_BUTTON_L2))
    }

    @Test fun adoptHeldKeyReconcilesHandoffOnRemappedButtonAndTriggerAxis() {
        // Both mode with L2 mapped to guest A (4): digital BUTTON_L2 arrives first, then LT moves to 0.25f (<= 0.50f).
        // Adopting the held key lets the axis reconcile pressed=false immediately so A never sticks when released.
        val remapped = AnalogTriggers.Sent()
        remapped.adoptHeldKey(4)
        assertEquals(listOf(Output(4, false, -1)), remapped.changes(AnalogTriggers.outputs(0.25f, 0f, 4, 15)))
        assertEquals(emptyList<Output>(), remapped.changes(AnalogTriggers.outputs(0f, 0f, 4, 15)))

        // If LT already passed 0.50f when taking over, no duplicate press is emitted and returning to rest releases A.
        remapped.adoptHeldKey(4)
        assertEquals(emptyList<Output>(), remapped.changes(AnalogTriggers.outputs(0.8f, 0f, 4, 15)))
        assertEquals(listOf(Output(4, false, -1)), remapped.changes(AnalogTriggers.outputs(0f, 0f, 4, 15)))

        // Standard LT mapping (14): adopting digital 255 reconciles to the real analog byte on the next sample.
        val standard = AnalogTriggers.Sent()
        standard.adoptHeldKey(14)
        assertEquals(listOf(Output(14, true, 64)), standard.changes(AnalogTriggers.outputs(0.25f, 0f, 14, 15)))
        assertEquals(listOf(Output(14, false, 0)), standard.changes(AnalogTriggers.outputs(0f, 0f, 14, 15)))
    }
}
