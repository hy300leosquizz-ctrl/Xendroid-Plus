package xendroid.compose.patches

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import xendroid.compose.patches.AspectRatioPatchHelper.Preset
import xendroid.compose.patches.AspectRatioPatchHelper.QuickFilter

class AspectRatioPatchHelperTest {

    @Test fun presetsProduceExactIeee754BigEndianHexConstants() {
        assertEquals("0x400E38E4", Preset.RATIO_20_9.be32Hex)
        assertEquals("0x4017B426", Preset.RATIO_21_9.be32Hex)
        assertEquals("0x400AAAAB", Preset.RATIO_19_5_9.be32Hex)
    }

    @Test fun clonesExisting16By9PatchIntoValidUltrawideToml() {
        val sampleToml = """
            title_name = "Halo 3"
            title_id = "4D5307E6"
            hash = ["1122334455667788"]

            [[patch]]
                name = "60 FPS Unlock"
                desc = "Unlocks frame rate."
                author = "canary"
                is_enabled = false

                [[patch.be32]]
                    address = 0x82100000
                    value = 0x38600001

            [[patch]]
                name = "16:9 Widescreen Aspect"
                desc = "Sets aspect ratio to 1.7777778."
                author = "canary"
                is_enabled = true

                [[patch.be32]]
                    address = 0x82200100
                    value = 0x3FE38E39
        """.trimIndent()

        assertTrue(AspectRatioPatchHelper.hasAdaptableTemplate(listOf(sampleToml)))
        val generated = AspectRatioPatchHelper.buildUltrawideToml(
            titleId = "4d5307e6",
            titleName = "Halo 3",
            sourceTexts = listOf(sampleToml),
            targetRatio = Preset.RATIO_20_9.ratio,
            ratioLabel = "20:9",
        )
        assertNotNull(generated)
        val parsed = PatchFileCheck.read(generated!!)
        assertEquals("4D5307E6", parsed.titleId)
        assertEquals(1, parsed.patches.size)
        assertTrue(parsed.patches.single().name.contains("(20:9)"))
        assertTrue(generated.contains("0x400E38E4"))
    }

    @Test fun customGuestAddressGeneratesValidTomlAndRejectsInvalidAddresses() {
        val generated = AspectRatioPatchHelper.buildUltrawideToml(
            titleId = "5454082B",
            titleName = "Red Dead Redemption",
            sourceTexts = emptyList(),
            targetRatio = Preset.RATIO_21_9.ratio,
            ratioLabel = "21:9",
            customAddress = "0x82012340",
        )
        assertNotNull(generated)
        val spec = PatchFileCheck.read(generated!!)
        assertEquals("5454082B", spec.titleId)
        assertEquals(0x82012340L, spec.patches.single().writes.single().address)
        assertTrue(generated.contains("0x4017B426"))

        assertNull(
            AspectRatioPatchHelper.buildUltrawideToml(
                titleId = "5454082B",
                titleName = "Red Dead Redemption",
                sourceTexts = emptyList(),
                targetRatio = Preset.RATIO_21_9.ratio,
                ratioLabel = "21:9",
                customAddress = "0x00001234",
            )
        )
    }

    @Test fun searchAndQuickFiltersMatchExpectedPatches() {
        val fpsPatch = PatchEntry(0, "60 FPS Unlock", "Doubles framerate", "boma", isEnabled = true)
        val widePatch = PatchEntry(1, "Ultrawide 21:9 FOV", "Changes aspect ratio", "helper", isEnabled = false)

        assertTrue(AspectRatioPatchHelper.matches(fpsPatch, "TU3", "", QuickFilter.ENABLED))
        assertFalse(AspectRatioPatchHelper.matches(widePatch, "TU3", "", QuickFilter.ENABLED))
        assertTrue(AspectRatioPatchHelper.matches(fpsPatch, "TU3", "", QuickFilter.FPS_60))
        assertFalse(AspectRatioPatchHelper.matches(widePatch, "TU3", "", QuickFilter.FPS_60))
        assertTrue(AspectRatioPatchHelper.matches(widePatch, "TU3", "helper", QuickFilter.WIDESCREEN))
        assertFalse(AspectRatioPatchHelper.matches(widePatch, "TU3", "missing", QuickFilter.ALL))
    }
}
