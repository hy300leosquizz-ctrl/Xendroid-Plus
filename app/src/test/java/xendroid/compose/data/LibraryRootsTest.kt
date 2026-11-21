package xendroid.compose.data

import org.junit.Assert.assertEquals
import org.junit.Test

class LibraryRootsTest {
    @Test fun theSingleFolderOfAnOlderInstallBecomesTheFirstRoot() {
        assertEquals(listOf("/storage/emulated/0/Games"), LibraryRoots.decode(null, "/storage/emulated/0/Games"))
        assertEquals(emptyList<String>(), LibraryRoots.decode(null, null))
        // Once the list exists it is the truth, even when empty (every folder removed).
        assertEquals(emptyList<String>(), LibraryRoots.decode("", "/storage/emulated/0/Games"))
        val roots = listOf("/sdcard/Xbox", "/storage/1234-5678/Games")
        assertEquals(roots, LibraryRoots.decode(LibraryRoots.encode(roots), "/old"))
        assertEquals(listOf("/a"), LibraryRoots.decode("/a\n/a\n \n", null))
    }

    @Test fun addingAndRemovingKeepTheOrderAndNeverDuplicate() {
        var roots = LibraryRoots.add(emptyList(), "/a")
        roots = LibraryRoots.add(roots, "/b")
        roots = LibraryRoots.add(roots, "/a")
        roots = LibraryRoots.add(roots, " ")
        assertEquals(listOf("/a", "/b"), roots)
        assertEquals(listOf("/b"), LibraryRoots.remove(roots, "/a"))
        assertEquals(roots, LibraryRoots.remove(roots, "/missing"))
    }

    @Test fun replacingAFolderKeepsItsPositionAndNeverDuplicates() {
        val roots = listOf("/install", "/sd/Games", "/usb/Games")
        // Editing the install folder keeps the replacement at index 0.
        assertEquals(listOf("/newInstall", "/sd/Games", "/usb/Games"), LibraryRoots.replace(roots, "/install", "/newInstall"))
        // Editing a middle folder replaces it in place.
        assertEquals(listOf("/install", "/sd/Xbox360", "/usb/Games"), LibraryRoots.replace(roots, "/sd/Games", "/sd/Xbox360"))
        // Replacing a secondary folder with an already-listed root collapses the duplicate without moving the install slot.
        assertEquals(listOf("/install", "/usb/Games"), LibraryRoots.replace(roots, "/sd/Games", "/install"))
        // Replacing the install folder with an already-listed secondary root promotes that root to index 0.
        assertEquals(listOf("/sd/Games", "/usb/Games"), LibraryRoots.replace(roots, "/install", "/sd/Games"))
        // Blank, identical, or unknown paths are no-ops.
        assertEquals(roots, LibraryRoots.replace(roots, "/install", " "))
        assertEquals(roots, LibraryRoots.replace(roots, "/install", "/install"))
        assertEquals(roots, LibraryRoots.replace(roots, "/missing", "/other"))
    }

    @Test fun theFolderThatReceivesInstallsMovesToTheTop() {
        val roots = listOf("/a", "/b", "/c")
        assertEquals(listOf("/c", "/a", "/b"), LibraryRoots.first(roots, "/c"))
        assertEquals(roots, LibraryRoots.first(roots, "/a"))
        // A folder that is not one of the library's is not added by it.
        assertEquals(roots, LibraryRoots.first(roots, "/elsewhere"))
    }

    @Test fun nestedRootsAreCoveredAndMissingOnesDoNotStopTheOthers() {
        // /sdcard is a symlink to /storage/emulated/0 on Android.
        val canonical = mapOf(
            "/sdcard/Games" to "/storage/emulated/0/Games",
            "/storage/emulated/0/Games/Xbox" to "/storage/emulated/0/Games/Xbox",
            "/storage/emulated/0/Games" to "/storage/emulated/0/Games",
            "/storage/emulated/0/GamesExtra" to "/storage/emulated/0/GamesExtra",
            "/storage/ABCD-1234/Xbox" to "/storage/ABCD-1234/Xbox",
        )
        val plan = LibraryRoots.plan(
            listOf("/storage/emulated/0/Games/Xbox", "/sdcard/Games", "/storage/emulated/0/Games",
                "/storage/emulated/0/GamesExtra", "/storage/ABCD-1234/Xbox", "/storage/gone"),
            canonical = { canonical[it] },
            readable = { it != "/storage/ABCD-1234/Xbox" },        // the SD card was removed
        )
        assertEquals(listOf("/sdcard/Games", "/storage/emulated/0/GamesExtra"), plan.scan)
        // Inside another root, or the same folder under a second name.
        assertEquals(listOf("/storage/emulated/0/Games/Xbox", "/storage/emulated/0/Games"), plan.covered)
        assertEquals(listOf("/storage/ABCD-1234/Xbox", "/storage/gone"), plan.unavailable)
    }
}
