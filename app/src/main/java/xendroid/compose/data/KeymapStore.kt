package xendroid.compose.data

import android.content.Context
import android.util.Log
import androidx.datastore.core.DataMigration
import androidx.datastore.core.DataStore
import androidx.datastore.core.Serializer
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.intPreferencesKey
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.concurrent.ConcurrentHashMap
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import xendroid.compose.gamepad.SharedStores

@Serializable
data class KeymapDto(
    val initialized: Boolean = false,
    val bindings: Map<Int, Int> = emptyMap(),
    val vibrate: Boolean? = null,
)

/**
 * Persists the 16-button hardware-key mapping + the vibrate toggle in a multi-process store
 * (`datastore/keymap.json` via [SharedStores]) so both the main process (`KeymapScreen` in
 * `Controls`) and `:emu` (`EmulatorHostActivity` in-game remapping + live collection) read and
 * write safely without sharing the single-process `xendroid_prefs` DataStore (#118).
 *
 * Existing bindings saved in `xendroid_prefs` (`keymap_0..keymap_15`, `enable_vibrator`) are
 * imported once on first access without mutating `xendroid_prefs`.
 */
class KeymapStore(private val appContext: Context) {
    private val store: DataStore<KeymapDto> = stores.computeIfAbsent(appContext.filesDir.absolutePath) {
        SharedStores.create(
            serializer = KeymapSerializer,
            migrations = listOf(legacyMigration(appContext)),
            file = { File(appContext.filesDir, "datastore/keymap.json") },
        )
    }

    private val data: Flow<KeymapDto> = store.data.catch {
        if (it is CancellationException) throw it
        Log.w("KeymapStore", "Keymap unreadable; defaults apply", it)
        emit(KeymapDto())
    }

    /** button index 0..15 -> bound Android keycode (default-filled where absent). */
    val bindings: Flow<Map<Int, Int>> = data.map { dto -> readBindings(dto.bindings) }

    /** Android keycode -> game KEY_CODE, ready for the host lookup. Unbound (0) dropped. */
    val androidToGameKey: Flow<Map<Int, Int>> = data.map { dto ->
        val out = HashMap<Int, Int>(GameButtons.ALL.size)
        for (b in GameButtons.ALL) {
            val code = dto.bindings[b.index] ?: b.defaultAndroidKey
            if (code != 0) out[code] = b.keyCode
        }
        out
    }

    val vibrateEnabled: Flow<Boolean> = data.map { it.vibrate ?: false }

    suspend fun setBinding(index: Int, androidKeyCode: Int) {
        store.updateData { dto ->
            dto.copy(initialized = true, bindings = dto.bindings + (index to androidKeyCode))
        }
    }

    /** 15o: several buttons in one write (a key traded between two buttons, the face-button swap). */
    suspend fun setBindings(changes: Map<Int, Int>) {
        if (changes.isEmpty()) return
        store.updateData { dto ->
            dto.copy(initialized = true, bindings = dto.bindings + changes)
        }
    }

    /** Clear a single binding (stores 0 = unbound, matching legacy "Clear"). */
    suspend fun clearBinding(index: Int) = setBinding(index, 0)

    /** Reset all 16 rows to KeyMapConfig.DEFAULT_KEYMAPPERS. */
    suspend fun resetToDefaults() {
        val defaults = GameButtons.ALL.associate { it.index to it.defaultAndroidKey }
        store.updateData { dto -> dto.copy(initialized = true, bindings = defaults) }
    }

    suspend fun setVibrate(enabled: Boolean) {
        store.updateData { dto -> dto.copy(initialized = true, vibrate = enabled) }
    }

    private fun readBindings(stored: Map<Int, Int>): Map<Int, Int> {
        val out = HashMap<Int, Int>(GameButtons.ALL.size)
        for (b in GameButtons.ALL) out[b.index] = stored[b.index] ?: b.defaultAndroidKey
        return out
    }

    companion object {
        private val stores = ConcurrentHashMap<String, DataStore<KeymapDto>>()

        private fun legacyMigration(appContext: Context) = object : DataMigration<KeymapDto> {
            override suspend fun shouldMigrate(currentData: KeymapDto): Boolean = !currentData.initialized
            override suspend fun migrate(currentData: KeymapDto): KeymapDto {
                val legacyFile = File(appContext.filesDir, "datastore/xendroid_prefs.preferences_pb")
                if (!legacyFile.isFile) return currentData.copy(initialized = true)
                val prefs = runCatching { appContext.dataStore.data.first() }.getOrNull()
                    ?: return currentData.copy(initialized = true)
                val imported = HashMap<Int, Int>()
                for (b in GameButtons.ALL) {
                    prefs[intPreferencesKey("keymap_${b.index}")]?.let { imported[b.index] = it }
                }
                val vib = prefs[booleanPreferencesKey("enable_vibrator")]
                return KeymapDto(
                    initialized = true,
                    bindings = imported + currentData.bindings,
                    vibrate = currentData.vibrate ?: vib,
                )
            }
            override suspend fun cleanUp() = Unit
        }
    }
}

internal object KeymapSerializer : Serializer<KeymapDto> {
    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }
    override val defaultValue = KeymapDto()
    override suspend fun readFrom(input: InputStream): KeymapDto {
        val bytes = ByteArrayOutputStream()
        val buffer = ByteArray(4096)
        while (true) {
            val n = input.read(buffer)
            if (n < 0) break
            require(bytes.size() + n <= 64 * 1024) { "Keymap data is too large" }
            bytes.write(buffer, 0, n)
        }
        return if (bytes.size() == 0) KeymapDto()
        else json.decodeFromString(KeymapDto.serializer(), bytes.toString("UTF-8"))
    }
    override suspend fun writeTo(t: KeymapDto, output: OutputStream) {
        output.write(json.encodeToString(KeymapDto.serializer(), t).toByteArray())
    }
}
