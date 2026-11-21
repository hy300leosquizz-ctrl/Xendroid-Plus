package xendroid.compose.data

import java.io.File
import java.util.Locale

/**
 * Discovers selectable `.xex` executables for multi-game compilations and multi-executable titles
 * (e.g., *The Orange Box*, *Devil May Cry HD Collection*, *Metal Gear Solid HD Collection*, or any
 * extracted `.xex` folder holding both campaign and multiplayer binaries).
 *
 * Pure JVM logic — safe to test without Android or native dependencies.
 */
object MultiXexScanner {

    data class XexTarget(
        /** Module path passed to `--launch_module=<modulePath>` (empty = `default.xex`). */
        val modulePath: String,
        val displayLabel: String,
    )

    private const val MAX_DEPTH = 3
    private const val MAX_VISITED = 1500

    /** Known multi-XEX compilation discs so `.iso`, `.zar` and `GOD` copies also get 1-tap sub-game launch! */
    private val COMPILATION_PRESETS: Map<String, List<XexTarget>> = mapOf(
        // The Orange Box
        "4541080F" to listOf(
            XexTarget("", "Launcher Menu (default.xex)"),
            XexTarget("hl2_xbox.xex", "Half-Life 2 / Episodes (hl2_xbox.xex)"),
            XexTarget("portal_xbox.xex", "Portal (portal_xbox.xex)"),
            XexTarget("tf_xbox.xex", "Team Fortress 2 (tf_xbox.xex)"),
        ),
        // Devil May Cry HD Collection
        "4343081E" to listOf(
            XexTarget("", "Collection Launcher (default.xex)"),
            XexTarget("dmc1.xex", "Devil May Cry (dmc1.xex)"),
            XexTarget("dmc2.xex", "Devil May Cry 2 (dmc2.xex)"),
            XexTarget("dmc3.xex", "Devil May Cry 3: Special Edition (dmc3.xex)"),
        ),
        // Metal Gear Solid HD Collection
        "4B4E0840" to listOf(
            XexTarget("", "Collection Menu (default.xex)"),
            XexTarget("mgs2.xex", "Metal Gear Solid 2: Sons of Liberty (mgs2.xex)"),
            XexTarget("mgs3.xex", "Metal Gear Solid 3: Snake Eater (mgs3.xex)"),
        ),
    )

    /**
     * Lists selectable `.xex` modules for a game at [launchUri] / [titleId].
     * Returns an empty list (or single entry) when only `default.xex` exists; returns `>= 2`
     * entries when multiple executables are available.
     */
    fun discover(launchUri: String?, titleId: String? = null): List<XexTarget> {
        val path = launchUri?.trim().orEmpty()
        if (path.isNotEmpty() && !path.startsWith("content://")) {
            val f = File(path)
            val rootDir = when {
                f.isDirectory -> f
                f.isFile && f.name.endsWith(".xex", ignoreCase = true) -> f.parentFile
                else -> null
            }
            if (rootDir != null && rootDir.isDirectory) {
                val scanned = scanDirectory(rootDir)
                if (scanned.size > 1) return scanned
            }
        }
        val cleanTid = titleId?.trim()?.uppercase(Locale.US)
        return if (cleanTid != null) COMPILATION_PRESETS[cleanTid].orEmpty() else emptyList()
    }

    /** Walks an extracted `.xex` game root up to [MAX_DEPTH] levels deep. */
    fun scanDirectory(rootDir: File): List<XexTarget> {
        if (!rootDir.isDirectory) return emptyList()
        val found = ArrayList<String>()
        var visited = 0

        fun walk(dir: File, prefix: String, depth: Int) {
            if (visited >= MAX_VISITED) return
            val children = dir.listFiles()?.sortedBy { it.name.lowercase(Locale.US) } ?: return
            for (child in children) {
                if (++visited > MAX_VISITED) return
                if (child.name.startsWith(".")) continue
                val rel = if (prefix.isEmpty()) child.name else "$prefix\\${child.name}"
                if (child.isFile && child.name.endsWith(".xex", ignoreCase = true)) {
                    found += rel
                } else if (child.isDirectory && depth < MAX_DEPTH && !child.name.endsWith(".data", ignoreCase = true)) {
                    walk(child, rel, depth + 1)
                }
            }
        }

        walk(rootDir, "", 0)
        if (found.size <= 1) return emptyList()

        val sorted = found.sortedWith(
            compareBy<String> { !it.equals("default.xex", ignoreCase = true) }
                .thenBy { it.count { c -> c == '\\' } }
                .thenBy { it.lowercase(Locale.US) }
        )
        return sorted.map { rel ->
            if (rel.equals("default.xex", ignoreCase = true)) {
                XexTarget("", "default.xex")
            } else {
                XexTarget(rel, rel.replace('\\', '/'))
            }
        }
    }
}
