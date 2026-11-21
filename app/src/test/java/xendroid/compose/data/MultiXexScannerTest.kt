package xendroid.compose.data

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import xendroid.compose.ui.compress.GameCompressViewModel

class MultiXexScannerTest {

    @Test fun singleDefaultXexReturnsEmptyWhileMultiXexFolderListsAllExecutables() {
        val root = Files.createTempDirectory("xex-game").toFile()
        val defaultXex = File(root, "default.xex").apply { writeText("xex2") }
        assertTrue(MultiXexScanner.discover(defaultXex.absolutePath).isEmpty())

        File(root, "mp.xex").writeText("xex2")
        val sub = File(root, "episode2").apply { mkdirs() }
        File(sub, "ep2.xex").writeText("xex2")
        val svodData = File(root, "00007000.data").apply { mkdirs() }
        File(svodData, "Data0000.xex").writeText("ignored")

        val targets = MultiXexScanner.discover(defaultXex.absolutePath)
        assertEquals(3, targets.size)
        assertEquals("", targets[0].modulePath)
        assertEquals("default.xex", targets[0].displayLabel)
        assertEquals("mp.xex", targets[1].modulePath)
        assertEquals("episode2\\ep2.xex", targets[2].modulePath)
        assertEquals("episode2/ep2.xex", targets[2].displayLabel)
    }

    @Test fun compilationTitlePresetsOfferSubGamesOnDiscAndZarCopies() {
        val orangeBox = MultiXexScanner.discover("/games/The Orange Box.zar", titleId = "4541080f")
        assertEquals(4, orangeBox.size)
        assertTrue(orangeBox.any { it.modulePath == "portal_xbox.xex" })

        val dmcHd = MultiXexScanner.discover("/games/DMC HD.iso", titleId = "4343081E")
        assertEquals(4, dmcHd.size)
        assertTrue(dmcHd.any { it.modulePath == "dmc3.xex" })
    }

    @Test fun zarCompressionTargetResolvesIsoXexFolderAndNestedGodSafely() {
        assertTrue(GameCompressViewModel.canCompressFormat(GameFormat.ISO))
        assertTrue(GameCompressViewModel.canCompressFormat(GameFormat.XEX_FOLDER))
        assertTrue(GameCompressViewModel.canCompressFormat(GameFormat.GOD))
        assertFalse(GameCompressViewModel.canCompressFormat(GameFormat.ZAR))

        val gamesDir = Files.createTempDirectory("games").toFile()

        val iso = File(gamesDir, "Forza Horizon.iso").apply { writeText("iso") }
        val isoTarget = GameCompressViewModel.resolveCompressionTarget(iso.absolutePath, "Forza Horizon")
        assertNotNull(isoTarget)
        assertEquals(iso, isoTarget!!.deletableSingleIso)
        assertEquals(gamesDir, isoTarget.outputDir)
        assertEquals("Forza Horizon", isoTarget.baseName)

        val xexDir = File(gamesDir, "Halo Reach").apply { mkdirs() }
        val xex = File(xexDir, "default.xex").apply { writeText("xex") }
        val xexTarget = GameCompressViewModel.resolveCompressionTarget(xex.absolutePath, "Halo: Reach")
        assertNotNull(xexTarget)
        assertNull(xexTarget!!.deletableSingleIso)
        assertEquals(xexDir.absolutePath, xexTarget.nativeInputPath)
        assertEquals(gamesDir, xexTarget.outputDir)
        assertEquals("Halo_ Reach", xexTarget.baseName)

        val godHeaderDir = File(gamesDir, "4D5307E6/00007000").apply { mkdirs() }
        val godHeader = File(godHeaderDir, "098123456789ABCD").apply { writeText("LIVE") }
        val godTarget = GameCompressViewModel.resolveCompressionTarget(godHeader.absolutePath, "Halo 3")
        assertNotNull(godTarget)
        assertNull(godTarget!!.deletableSingleIso)
        assertEquals(gamesDir, godTarget.outputDir)
        assertEquals("Halo 3", godTarget.baseName)

        val alreadyZar = File(gamesDir, "Done.zar").apply { writeText("zar") }
        assertNull(GameCompressViewModel.resolveCompressionTarget(alreadyZar.absolutePath))
        assertNull(GameCompressViewModel.resolveCompressionTarget("content://authority/tree/game"))
    }
}
