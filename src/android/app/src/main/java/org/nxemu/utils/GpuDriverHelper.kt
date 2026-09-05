package org.nxemu.utils

import java.io.File
import java.io.IOException
import org.json.JSONObject
import org.nxemu.NXCoreSetting
import org.nxemu.NativeLibrary
import org.nxemu.NxEmuApplication

object GpuDriverHelper {
    private const val META_JSON_FILENAME = "meta.json"

    fun initializeDriverParameters() {
        val context = NxEmuApplication.appContext
        val filesDir = context.filesDir
        val publicDir = context.getExternalFilesDir(null) ?: filesDir

        val fileRedirectDir = File(publicDir, "gpu/vk_file_redirect")
        val driverInstallDir = File(filesDir, "gpu_driver")
        fileRedirectDir.mkdirs()
        driverInstallDir.mkdirs()

        val hookLibDir = context.applicationInfo.nativeLibraryDir.trimEnd('/') + "/"
        NativeLibrary.setSettingString(NXCoreSetting.GpuHookLibDir, hookLibDir)
        NativeLibrary.setSettingString(
            NXCoreSetting.GpuCustomDriverDir,
            canonicalPath(driverInstallDir).trimEnd('/') + "/"
        )
        NativeLibrary.setSettingString(
            NXCoreSetting.GpuFileRedirectDir,
            canonicalPath(fileRedirectDir).trimEnd('/') + "/"
        )
        NativeLibrary.setSettingString(
            NXCoreSetting.GpuCustomDriverName,
            installedCustomDriverLibraryName(driverInstallDir)
        )
    }

    private fun installedCustomDriverLibraryName(driverInstallDir: File): String {
        val meta = File(driverInstallDir, META_JSON_FILENAME)
        if (!meta.isFile) {
            return ""
        }
        return try {
            JSONObject(meta.readText()).optString("libraryName", "")
        } catch (_: Exception) {
            ""
        }
    }

    private fun canonicalPath(file: File): String {
        return try {
            file.canonicalPath
        } catch (_: IOException) {
            file.absolutePath
        }
    }
}
