package xendroid.compose.driver

import android.os.Build
import java.io.File
import xendroid.compose.core.EmulatorRuntime

/**
 * Whether this device can load a custom Vulkan driver package (.zip):
 *  - Qualcomm Adreno (Turnip / proprietary blobs via libadrenotools, /dev/kgsl-3d0)
 *  - Arm Mali / Immortalis (Mesa PanVK / updated Valhall-5thGen ICDs, /dev/mali0)
 *  - Samsung Xclipse (Exynos RDNA2/RDNA3 Vulkan ICDs / ExynosTools, /dev/sgpu or Xclipse GPU)
 */
object CustomDrivers {
    enum class VendorFamily { ADRENO, MALI, XCLIPSE, OTHER }

    /** The screen tests draw an Adreno phone on the JVM, which has no GPU; null = ask the device. */
    @Volatile var forced: Boolean? = null

    fun detectVendor(
        gpuName: String?,
        hasKgsl: Boolean = false,
        hasMaliNode: Boolean = false,
        hasXclipseNode: Boolean = false,
        socModel: String? = null,
    ): VendorFamily {
        val gpu = gpuName.orEmpty()
        val soc = socModel.orEmpty()
        return when {
            hasKgsl || gpu.contains("Adreno", ignoreCase = true) || gpu.contains("Turnip", ignoreCase = true) ->
                VendorFamily.ADRENO
            hasXclipseNode || gpu.contains("Xclipse", ignoreCase = true) ||
                Regex("(?i)^s5e(9925|9945|9955|8825|8835|8845|8855)").containsMatchIn(soc) ->
                VendorFamily.XCLIPSE
            hasMaliNode || gpu.contains("Mali", ignoreCase = true) ||
                gpu.contains("Immortalis", ignoreCase = true) || gpu.contains("PanVK", ignoreCase = true) ->
                VendorFamily.MALI
            else -> VendorFamily.OTHER
        }
    }

    fun isSupportedVendor(family: VendorFamily): Boolean = family != VendorFamily.OTHER

    val vendorFamily: VendorFamily
        get() = runCatching {
            val kgsl = File("/dev/kgsl-3d0").exists()
            val mali = File("/dev/mali0").exists() || File("/sys/class/misc/mali0").exists()
            val xclipse = File("/dev/sgpu").exists() ||
                runCatching {
                    File("/sys/kernel/gpu/gpu_model").takeIf { it.canRead() }?.readText()
                        ?.contains("Xclipse", ignoreCase = true) == true
                }.getOrDefault(false)
            val soc = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) Build.SOC_MODEL else Build.HARDWARE
            detectVendor(EmulatorRuntime.gpuDeviceName, kgsl, mali, xclipse, soc)
        }.getOrDefault(VendorFamily.OTHER)

    val supported: Boolean
        get() = forced ?: isSupportedVendor(vendorFamily)
}
