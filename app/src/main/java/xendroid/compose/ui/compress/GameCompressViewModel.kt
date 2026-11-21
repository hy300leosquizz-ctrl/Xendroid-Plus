package xendroid.compose.ui.compress

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import xendroid.compose.R
import xendroid.compose.core.EmulatorRuntime
import xendroid.compose.data.GameFormat

/**
 * Drives the per-game "Compress to .zar" safe-replace flow across `.iso` disc images, extracted
 * `.xex` game folders, and `GOD/SVOD` containers, backed by [ZarCompressService] so Android keeps
 * the worker alive while multi-core Zstandard compression runs in the background.
 *
 * Deleting the original source is offered ONLY when the source is a single `.iso` file and ONLY
 * after a verified `.zar` has been placed beside it; extracted `.xex` folders and multi-file
 * `GOD/SVOD` containers keep their original trees untouched.
 */
class GameCompressViewModel(
    private val appContext: Context,
) : ViewModel() {

    sealed interface CompressState {
        data object Idle : CompressState
        data class Busy(val message: String, val progress: Float = -1f) : CompressState
        /** The verified .zar is in place and the .iso is STILL there; the user decides. */
        data class ConfirmDelete(
            val isoPath: String,
            val isoName: String,
            val zarName: String,
            val isoBytes: Long,
        ) : CompressState
        data class Done(val message: String) : CompressState
        /** Any failure; the source is untouched. */
        data class Failed(val message: String) : CompressState
    }

    private val _state = MutableStateFlow<CompressState>(CompressState.Idle)
    val state: StateFlow<CompressState> = _state.asStateFlow()

    fun dismiss() { _state.value = CompressState.Idle }

    /** Compress the game at [launchUri] into a verified `.zar` archive beside it. */
    fun compress(launchUri: String, gameName: String? = null) = viewModelScope.launch {
        val compressing = appContext.getString(R.string.cz_compressing)
        val notifyTitle = gameName?.takeIf { it.isNotBlank() } ?: compressing
        _state.value = CompressState.Busy(compressing, 0f)
        ZarCompressService.notifyStart(appContext, notifyTitle)
        var lastNotifiedPct = -1
        val poll = launch {
            while (isActive) {
                val p = (EmulatorRuntime.emulator?.compressProgress() ?: 0f).coerceIn(0f, 1f)
                (_state.value as? CompressState.Busy)
                    ?.takeIf { it.message == compressing }
                    ?.let { _state.value = it.copy(progress = p) }
                val pct = (p * 100f).toInt().coerceIn(0, 100)
                if (pct != lastNotifiedPct) {
                    lastNotifiedPct = pct
                    ZarCompressService.notifyProgress(appContext, notifyTitle, pct)
                }
                delay(250)
            }
        }
        val result = try {
            withContext(Dispatchers.IO) { runCompress(launchUri, gameName) }
        } finally {
            poll.cancel()
            ZarCompressService.notifyStop(appContext)
        }
        _state.value = result
    }

    fun deleteIso() = viewModelScope.launch {
        val s = _state.value as? CompressState.ConfirmDelete ?: return@launch
        val deleted = withContext(Dispatchers.IO) { File(s.isoPath).delete() }
        _state.value = CompressState.Done(
            if (deleted) appContext.getString(R.string.cz_replaced, s.zarName)
            else appContext.getString(R.string.cz_iso_not_deleted, s.zarName)
        )
    }

    /** Keep both files. Also the dismiss path, so a stray tap outside the dialog can never delete anything. */
    fun keepIso() {
        val s = _state.value as? CompressState.ConfirmDelete ?: return
        _state.value = CompressState.Done(appContext.getString(R.string.cz_kept, s.zarName))
    }

    private suspend fun runCompress(launchUri: String, gameName: String?): CompressState {
        EmulatorRuntime.ensureLoaded()
        val emulator = EmulatorRuntime.emulator
            ?: return CompressState.Failed(appContext.getString(R.string.cz_not_loaded))

        val target = resolveCompressionTarget(launchUri, gameName)
            ?: return CompressState.Failed(appContext.getString(R.string.cz_cannot_open))
        if (!target.outputDir.canWrite()) {
            return CompressState.Failed(appContext.getString(R.string.cz_not_writable))
        }
        val zarName = "${target.baseName}.zar"
        if (File(target.outputDir, zarName).exists()) {
            return CompressState.Failed(appContext.getString(R.string.cz_exists, zarName))
        }

        val tempZar = File.createTempFile("compress", ".zar", appContext.cacheDir)
        tempZar.delete() // native creates the output; it must not pre-exist
        var placed: File? = null
        try {
            val packStatus = emulator.compressIsoToZar(target.nativeInputPath, tempZar.absolutePath)
            if (packStatus != 0) {
                return CompressState.Failed(
                    appContext.getString(R.string.cz_failed_code, packStatus.toUInt().toString(16))
                )
            }

            _state.value = CompressState.Busy(appContext.getString(R.string.cz_replacing))
            val dest = File(target.outputDir, zarName)
            val copied = runCatching { tempZar.copyTo(dest, overwrite = false) }.isSuccess
            if (!copied) {
                dest.delete(); placed = null
                return CompressState.Failed(appContext.getString(R.string.cz_cannot_write))
            }
            placed = dest
            if (dest.length() <= 0L || dest.length() != tempZar.length()) {
                dest.delete(); placed = null
                return CompressState.Failed(appContext.getString(R.string.cz_incomplete))
            }

            return if (target.deletableSingleIso != null) {
                CompressState.ConfirmDelete(
                    isoPath = target.deletableSingleIso.absolutePath,
                    isoName = target.deletableSingleIso.name,
                    zarName = zarName,
                    isoBytes = target.deletableSingleIso.length(),
                )
            } else {
                CompressState.Done(appContext.getString(R.string.cz_kept, zarName))
            }
        } catch (e: Exception) {
            placed?.delete()
            return CompressState.Failed(appContext.getString(R.string.cz_failed, e.message ?: e.javaClass.simpleName))
        } finally {
            tempZar.delete()
        }
    }

    data class CompressionTarget(
        val nativeInputPath: String,
        val outputDir: File,
        val baseName: String,
        val deletableSingleIso: File?,
    )

    companion object {
        fun canCompressFormat(format: GameFormat): Boolean =
            format == GameFormat.ISO || format == GameFormat.XEX_FOLDER || format == GameFormat.GOD

        private fun sanitizeBaseName(raw: String): String =
            raw.replace(Regex("[\\\\/:*?\"<>|]"), "_").trim().trim('.').take(120).ifBlank { "game" }

        /** Pure resolution of `.iso`, extracted `.xex` folder, or `GOD/SVOD` container paths. */
        fun resolveCompressionTarget(launchUri: String, gameName: String? = null): CompressionTarget? {
            if (launchUri.isBlank() || launchUri.startsWith("content://")) return null
            val f = File(launchUri)
            return when {
                f.isFile && f.name.endsWith(".iso", ignoreCase = true) -> {
                    val outDir = f.parentFile ?: return null
                    val base = sanitizeBaseName(f.name.substringBeforeLast('.'))
                    CompressionTarget(f.absolutePath, outDir, base, deletableSingleIso = f)
                }
                f.isFile && f.name.endsWith(".xex", ignoreCase = true) -> {
                    val gameDir = f.parentFile ?: return null
                    val outDir = gameDir.parentFile ?: gameDir
                    val base = sanitizeBaseName(gameName?.takeIf { it.isNotBlank() } ?: gameDir.name)
                    CompressionTarget(gameDir.absolutePath, outDir, base, deletableSingleIso = null)
                }
                f.isDirectory && File(f, "default.xex").exists() -> {
                    val outDir = f.parentFile ?: f
                    val base = sanitizeBaseName(gameName?.takeIf { it.isNotBlank() } ?: f.name)
                    CompressionTarget(f.absolutePath, outDir, base, deletableSingleIso = null)
                }
                f.isFile && !f.name.endsWith(".zar", ignoreCase = true) -> {
                    // GOD / SVOD / STFS container: step above `<TitleID>/<00007000>/<Header>` when nested.
                    val typeDir = f.parentFile ?: return null
                    val titleDir = typeDir.parentFile
                    val rootDir = if (typeDir.name.matches(Regex("(?i)[0-9a-f]{8}")) &&
                        titleDir?.name?.matches(Regex("(?i)[0-9a-f]{8}")) == true
                    ) {
                        titleDir.parentFile ?: titleDir
                    } else {
                        typeDir
                    }
                    val base = sanitizeBaseName(gameName?.takeIf { it.isNotBlank() } ?: f.name)
                    CompressionTarget(f.absolutePath, rootDir, base, deletableSingleIso = null)
                }
                else -> null
            }
        }
    }
}
