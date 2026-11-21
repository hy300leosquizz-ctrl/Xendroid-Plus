package xendroid.compose.driver

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContract
import java.io.File
import xendroid.compose.core.EmulatorRuntime

/**
 * Isolated `:driver_probe` process check for imported or downloaded custom `.zip` Vulkan drivers
 * (Turnip on Adreno, PanVK on Arm Mali/Immortalis, ExynosTools on Samsung Xclipse).
 *
 * Loading an incompatible or damaged Vulkan ICD directly in the main process or `:emu` can trigger
 * an unrecoverable native `SIGSEGV` / `SIGABRT` inside `dlopen`, `vkCreateInstance`, or
 * `vkCreateDevice`. Running that handshake inside `:driver_probe` guarantees that a crash dies
 * harmlessly in the child process (`RESULT_CANCELED`), keeping the UI alive and preventing a
 * broken driver from ever being persisted as the active `vulkan_lib_path`.
 */
class DriverProbeActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val path = intent?.getStringExtra(EXTRA_DRIVER_PATH).orEmpty()
        val root = runCatching { xendroid.compose.Application.get_custom_driver_dir() }.getOrNull()
        if (path.isBlank() || !isAllowedDriverFile(path, root)) {
            finishWithResult(ProbeOutcome.Failed("Driver library is outside the verified driver store"))
            return
        }
        val outcome = try {
            EmulatorRuntime.ensureLoaded()
            EmulatorRuntime.emulator?.setup_context(applicationContext)
            val nativeLibDir = applicationInfo?.nativeLibraryDir.orEmpty()
            val raw = nativeProbeVulkanDriver(path, nativeLibDir)
            parseNativeProbeReply(raw)
        } catch (t: Throwable) {
            ProbeOutcome.Failed(t.message ?: t.javaClass.simpleName)
        }
        finishWithResult(outcome)
    }

    private fun finishWithResult(outcome: ProbeOutcome) {
        val data = Intent().apply {
            putExtra(EXTRA_PROBE_OK, outcome is ProbeOutcome.Passed)
            when (outcome) {
                is ProbeOutcome.Passed -> {
                    putExtra(EXTRA_DEVICE_NAME, outcome.deviceName)
                    putExtra(EXTRA_API_VERSION, outcome.apiVersion)
                }
                is ProbeOutcome.Failed -> {
                    putExtra(EXTRA_ERROR_REASON, outcome.reason)
                }
            }
        }
        setResult(Activity.RESULT_OK, data)
        finish()
        overridePendingTransition(0, 0)
    }

    sealed interface ProbeOutcome {
        data class Passed(val deviceName: String, val apiVersion: String) : ProbeOutcome
        data class Failed(val reason: String) : ProbeOutcome
    }

    class Contract : ActivityResultContract<String, ProbeOutcome>() {
        override fun createIntent(context: Context, input: String): Intent =
            Intent(context, DriverProbeActivity::class.java).putExtra(EXTRA_DRIVER_PATH, input)

        override fun parseResult(resultCode: Int, intent: Intent?): ProbeOutcome {
            if (resultCode != Activity.RESULT_OK || intent == null) {
                return ProbeOutcome.Failed("Driver crashed during isolated Vulkan probe (vkCreateInstance/vkCreateDevice)")
            }
            return if (intent.getBooleanExtra(EXTRA_PROBE_OK, false)) {
                ProbeOutcome.Passed(
                    deviceName = intent.getStringExtra(EXTRA_DEVICE_NAME).orEmpty().ifBlank { "Vulkan GPU" },
                    apiVersion = intent.getStringExtra(EXTRA_API_VERSION).orEmpty().ifBlank { "1.0" },
                )
            } else {
                ProbeOutcome.Failed(
                    intent.getStringExtra(EXTRA_ERROR_REASON).orEmpty().ifBlank { "Vulkan device creation failed" }
                )
            }
        }
    }

    companion object {
        const val EXTRA_DRIVER_PATH = "driver_path"
        const val EXTRA_PROBE_OK = "probe_ok"
        const val EXTRA_DEVICE_NAME = "device_name"
        const val EXTRA_API_VERSION = "api_version"
        const val EXTRA_ERROR_REASON = "error_reason"

        /** Pure check: only `.so` files installed inside the app's private `driver/` root may be probed. */
        fun isAllowedDriverFile(path: String, driverRoot: File?): Boolean {
            if (driverRoot == null || !path.endsWith(".so")) return false
            return runCatching {
                val target = File(path).canonicalFile
                val base = driverRoot.canonicalFile
                target.isFile && target.toPath().startsWith(base.toPath()) && target != base
            }.getOrDefault(false)
        }

        /** Pure parser for `"OK|<deviceName>|<apiVersion>"` or `"ERR|<reason>"` from JNI. */
        fun parseNativeProbeReply(raw: String?): ProbeOutcome {
            if (raw.isNullOrBlank()) return ProbeOutcome.Failed("Empty response from native Vulkan probe")
            val parts = raw.split('|', limit = 3)
            return when (parts.firstOrNull()) {
                "OK" -> ProbeOutcome.Passed(
                    deviceName = parts.getOrNull(1)?.trim().orEmpty().ifEmpty { "Vulkan GPU" },
                    apiVersion = parts.getOrNull(2)?.trim().orEmpty().ifEmpty { "1.0" },
                )
                "ERR" -> ProbeOutcome.Failed(
                    parts.getOrNull(1)?.trim().orEmpty().ifEmpty { "Vulkan probe failed" }
                )
                else -> ProbeOutcome.Failed(raw.take(200))
            }
        }

        @JvmStatic
        private external fun nativeProbeVulkanDriver(driverPath: String, nativeLibDir: String): String
    }
}
