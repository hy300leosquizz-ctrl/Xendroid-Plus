package xendroid.compose.patches

import java.util.Locale

/**
 * Clean-room widescreen / ultrawide aspect-ratio patch helper for modern Android displays
 * (19.5:9, 20:9, 21:9, or arbitrary screen dimensions).
 *
 * Computes IEEE-754 big-endian `be32` (`0x400E38E4` for 20:9, `0x4017B426` for 21:9) and `f32`
 * values, and can clone an existing 16:9 / widescreen `[[patch]]` entry from a title's `.patch.toml`
 * into a schema-validated user `.patch.toml` ready for `PatchStore.importUserPatch`.
 */
object AspectRatioPatchHelper {

    enum class Preset(val label: String, val widthRatio: Float, val heightRatio: Float) {
        RATIO_19_5_9("19.5:9", 19.5f, 9f),
        RATIO_20_9("20:9", 20f, 9f),
        // A "21:9" ultrawide panel is 2560x1080, i.e. 64:27 (2.3703704). The nominal
        // 21:9 (2.3333333) belongs to 2520x1080 phones and produces the wrong stretch.
        RATIO_21_9("21:9", 64f, 27f),
        ;

        val ratio: Float get() = widthRatio / heightRatio
        val be32Hex: String get() = floatToBe32Hex(ratio)
    }

    /** IEEE-754 single-precision float formatted as an 8-digit uppercase hex literal (`0x400E38E4`). */
    fun floatToBe32Hex(ratio: Float): String {
        require(ratio.isFinite() && ratio in 1.0f..5.0f) { "Aspect ratio must be finite between 1.0 and 5.0" }
        val bits = java.lang.Float.floatToRawIntBits(ratio)
        return String.format(Locale.US, "0x%08X", bits)
    }

    /** Matches search queries and quick filter chips in `GamePatchesScreen`. */
    enum class QuickFilter { ALL, ENABLED, FPS_60, WIDESCREEN }

    fun matches(entry: PatchEntry, fileVariant: String, query: String, filter: QuickFilter): Boolean {
        val hay = "${entry.name} ${entry.desc.orEmpty()} ${entry.author.orEmpty()} $fileVariant"
        val filterOk = when (filter) {
            QuickFilter.ALL -> true
            QuickFilter.ENABLED -> entry.isEnabled
            QuickFilter.FPS_60 -> Regex("(?i)(60\\s*fps|unlock\\s*fps|frame\\s*rate|framerate|vblank)").containsMatchIn(hay)
            QuickFilter.WIDESCREEN -> Regex("(?i)(aspect|widescreen|ultrawide|16:9|20:9|21:9|fov)").containsMatchIn(hay)
        }
        if (!filterOk) return false
        val q = query.trim()
        return q.isEmpty() || hay.contains(q, ignoreCase = true)
    }

    private val ASPECT_16_9_HEX = Regex("(?i)0x3fe38e3[89a]")
    private val ASPECT_16_9_FLOAT = Regex("(?<![0-9.])1\\.777[0-9]*")

    /**
     * Builds a validated `.patch.toml` adapting an existing 16:9 aspect-ratio `[[patch]]` in
     * [sourceTexts] to [targetRatio], or creates a single-write `be32` patch when [customAddress]
     * (`0x82......`) is supplied. Returns null if neither a 16:9 template nor a valid address exists.
     */
    fun buildUltrawideToml(
        titleId: String,
        titleName: String,
        sourceTexts: List<String>,
        targetRatio: Float,
        ratioLabel: String,
        customAddress: String? = null,
    ): String? {
        val cleanTitleId = titleId.trim().uppercase(Locale.US)
        if (!cleanTitleId.matches(Regex("[0-9A-F]{8}"))) return null
        val safeTitle = titleName.replace("\\", "").replace("\"", "'").trim().ifEmpty { cleanTitleId }
        val hexValue = floatToBe32Hex(targetRatio)
        val floatLiteral = String.format(Locale.US, "%.7f", targetRatio)

        val addrTrimmed = customAddress?.trim().orEmpty()
        if (addrTrimmed.isNotEmpty()) {
            val cleanHex = addrTrimmed.removePrefix("0x").removePrefix("0X")
            val parsedAddr = cleanHex.toLongOrNull(16)?.takeIf { it in 0x80000000L..0x9FFFFFFFL } ?: return null
            val hashList = extractFirstHashes(sourceTexts).ifEmpty { listOf("0000000000000000") }
            val hashesToml = hashList.joinToString(", ") { "\"$it\"" }
            val addrFormatted = String.format(Locale.US, "0x%08X", parsedAddr)
            return buildString {
                appendLine("title_name = \"$safeTitle\"")
                appendLine("title_id = \"$cleanTitleId\"")
                appendLine("hash = [$hashesToml]")
                appendLine()
                appendLine("[[patch]]")
                appendLine("    name = \"$ratioLabel Ultrawide Aspect Ratio\"")
                appendLine("    desc = \"Overrides aspect ratio float to $ratioLabel ($floatLiteral / $hexValue).\"")
                appendLine("    author = \"Xendroid+ Aspect Helper\"")
                appendLine("    is_enabled = false")
                appendLine()
                appendLine("    [[patch.be32]]")
                appendLine("        address = $addrFormatted")
                appendLine("        value = $hexValue")
            }.takeIf { runCatching { PatchFileCheck.read(it) }.isSuccess }
        }

        for (toml in sourceTexts) {
            val spec = runCatching { PatchFileCheck.read(toml) }.getOrNull() ?: continue
            val blocks = splitPatchBlocks(toml)
            for (block in blocks) {
                if (ASPECT_16_9_HEX.containsMatchIn(block) || ASPECT_16_9_FLOAT.containsMatchIn(block)) {
                    val rewritten = block
                        .replace(ASPECT_16_9_HEX, hexValue)
                        .replace(ASPECT_16_9_FLOAT, floatLiteral)
                        .replace(
                            Regex("(?m)^(\\s*name\\s*=\\s*\")([^\"]*)(\".*)$"),
                            "$1$2 ($ratioLabel)$3",
                        )
                    val hashesToml = spec.hashes.joinToString(", ") { "\"$it\"" }
                    val candidate = buildString {
                        appendLine("title_name = \"${spec.titleName.replace("\\", "").replace("\"", "'")}\"")
                        appendLine("title_id = \"${spec.titleId}\"")
                        appendLine("hash = [$hashesToml]")
                        appendLine()
                        append(rewritten.trimEnd())
                        appendLine()
                    }
                    if (runCatching { PatchFileCheck.read(candidate) }.isSuccess) {
                        return candidate
                    }
                }
            }
        }
        return null
    }

    /** True if any loaded patch file for this title has a 16:9 float (`0x3FE38E39` / `1.777...`) we can auto-adapt. */
    fun hasAdaptableTemplate(sourceTexts: List<String>): Boolean =
        sourceTexts.any { ASPECT_16_9_HEX.containsMatchIn(it) || ASPECT_16_9_FLOAT.containsMatchIn(it) }

    private fun extractFirstHashes(sourceTexts: List<String>): List<String> {
        for (toml in sourceTexts) {
            val spec = runCatching { PatchFileCheck.read(toml) }.getOrNull()
            if (spec != null && spec.hashes.isNotEmpty()) return spec.hashes
        }
        return emptyList()
    }

    private fun splitPatchBlocks(toml: String): List<String> {
        val lines = toml.replace("\r\n", "\n").split('\n')
        val out = ArrayList<String>()
        var current: StringBuilder? = null
        for (line in lines) {
            if (Regex("^\\s*\\[\\[\\s*patch\\s*]]\\s*(#.*)?$").matches(line)) {
                current?.let { out += it.toString() }
                current = StringBuilder().appendLine(line)
            } else {
                current?.appendLine(line)
            }
        }
        current?.let { out += it.toString() }
        return out
    }
}
