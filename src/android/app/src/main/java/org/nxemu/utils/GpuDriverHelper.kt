package org.nxemu.utils

import org.nxemu.NXCoreSetting
import org.nxemu.NativeLibrary
import org.nxemu.NxEmuApplication
import java.io.File
import java.io.IOException

object GpuDriverHelper {
    fun initializeDriverParameters() {
        val context = NxEmuApplication.appContext
        val driverInstallationPath = try {
            context.filesDir.canonicalPath + "/gpu_driver/"
        } catch (e: IOException) {
            throw RuntimeException(e)
        }

        File(driverInstallationPath).mkdirs()

        val hookLibPath = context.applicationInfo.nativeLibraryDir + "/"
        NativeLibrary.setSettingString(NXCoreSetting.GpuHookLibDir, hookLibPath)
        NativeLibrary.setSettingString(NXCoreSetting.GpuCustomDriverDir, driverInstallationPath)
        NativeLibrary.setSettingString(
            NXCoreSetting.GpuCustomDriverName,
            installedCustomDriverName(driverInstallationPath),
        )
        NativeLibrary.setSettingString(NXCoreSetting.GpuFileRedirectDir, "")
    }

    private fun installedCustomDriverName(installDir: String): String {
        val meta = File(installDir, "meta.json")
        if (!meta.isFile) {
            return ""
        }
        return try {
            val json = meta.readText()
            Regex(""""libraryName"\s*:\s*"([^"]+)"""").find(json)?.groupValues?.get(1) ?: ""
        } catch (_: Exception) {
            ""
        }
    }
}
