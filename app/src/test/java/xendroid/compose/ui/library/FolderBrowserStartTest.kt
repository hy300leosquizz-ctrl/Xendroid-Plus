package xendroid.compose.ui.library

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Test

class FolderBrowserStartTest {
    @Test fun startsAtReadableParentOrFallsBackToExistingAncestor() {
        val root = Files.createTempDirectory("xd-folders").toFile()
        try {
            val games = File(root, "Games").apply { mkdirs() }
            // Existing folder with a readable parent opens at the parent so sibling folders are visible.
            assertEquals(root.canonicalFile, folderBrowserStartFor(games.absolutePath)?.canonicalFile)
            // Missing nested folder walks up to the nearest readable ancestor.
            val gone = File(games, "Moved/Away")
            assertEquals(games.canonicalFile, folderBrowserStartFor(gone.absolutePath)?.canonicalFile)
        } finally {
            root.deleteRecursively()
        }
    }
}
