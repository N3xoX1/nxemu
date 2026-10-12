package org.nxemu.utils

import android.content.Context
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import org.json.JSONArray
import org.json.JSONObject
import org.nxemu.NXCoreSetting
import org.nxemu.NativeLibrary
import org.nxemu.NxEmuApplication
import java.io.File
import java.util.zip.ZipFile
import java.util.zip.ZipInputStream

object GpuDriverHelper {
    private const val META_JSON = "meta.json"
    private const val MAX_META_BYTES = 500_000

    fun supportsCustomDrivers(): Boolean {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && File("/dev/kgsl-3d0").exists()
    }

    fun activeDriverLabel(): String {
        val name = readMeta(File(installDir(), META_JSON))?.optString("name").orEmpty()
        return if (name.isEmpty()) "System" else name
    }

    fun listDriversJson(): String {
        val installed = readMeta(File(installDir(), META_JSON))
        val drivers = JSONArray()
        storageDir().listFiles()?.sortedBy { it.name.lowercase() }?.forEach { zip ->
            if (!zip.isFile || !zip.name.endsWith(".zip", ignoreCase = true)) {
                return@forEach
            }
            val meta = readMetaFromZip(zip) ?: return@forEach
            val name = meta.optString("name")
            if (name.isEmpty()) {
                return@forEach
            }
            drivers.put(JSONObject()
                .put("file", zip.name)
                .put("name", name)
                .put("version", meta.optString("driverVersion"))
                .put("selected", installed != null && sameDriver(installed, meta)))
        }
        return JSONObject()
            .put("supported", supportsCustomDrivers())
            .put("active", activeDriverLabel())
            .put("drivers", drivers)
            .toString()
    }

    fun initializeDriverParameters() {
        val context = NxEmuApplication.appContext
        val install = installDir()
        install.mkdirs()
        storageDir().mkdirs()

        val hookLibPath = context.applicationInfo.nativeLibraryDir + "/"
        NativeLibrary.setSettingString(NXCoreSetting.GpuHookLibDir, hookLibPath)
        NativeLibrary.setSettingString(NXCoreSetting.GpuCustomDriverDir, install.canonicalPath + "/")
        NativeLibrary.setSettingString(
            NXCoreSetting.GpuCustomDriverName,
            readMeta(File(install, META_JSON))?.optString("libraryName").orEmpty(),
        )
        val redirect = context.getExternalFilesDir(null)?.let { File(it, "gpu/vk_file_redirect") }
        if (redirect != null) {
            redirect.mkdirs()
            NativeLibrary.setSettingString(NXCoreSetting.GpuFileRedirectDir, redirect.canonicalPath + "/")
        } else {
            NativeLibrary.setSettingString(NXCoreSetting.GpuFileRedirectDir, "")
        }
    }

    fun useSystemDriver() {
        clearInstallDir()
        initializeDriverParameters()
    }

    fun useStoredDriver(fileName: String): Boolean {
        val zip = storedZip(fileName) ?: return false
        val meta = readMetaFromZip(zip) ?: return false
        if (!apiAllowed(meta)) {
            return false
        }
        return installZip(zip)
    }

    fun importAndSelect(context: Context, uri: Uri): String {
        if (!supportsCustomDrivers()) {
            return "unsupported"
        }
        storageDir().mkdirs()
        val fileName = safeZipName(displayName(context, uri))
        val dest = File(storageDir(), fileName)
        try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                dest.outputStream().use { output -> input.copyTo(output) }
            } ?: return "copy"
        } catch (_: Exception) {
            dest.delete()
            return "copy"
        }
        return validateAndInstall(dest)
    }

    fun importDownloaded(source: File, fileName: String): String {
        if (!supportsCustomDrivers()) {
            source.delete()
            return "unsupported"
        }
        storageDir().mkdirs()
        val dest = File(storageDir(), safeZipName(fileName))
        try {
            source.copyTo(dest, overwrite = true)
        } catch (_: Exception) {
            dest.delete()
            return "copy"
        } finally {
            source.delete()
        }
        return validateAndInstall(dest)
    }

    fun isStored(fileName: String): Boolean {
        return File(storageDir(), safeZipName(fileName)).isFile
    }

    fun storedName(fileName: String): String = safeZipName(fileName)

    private fun validateAndInstall(dest: File): String {
        val meta = readMetaFromZip(dest)
        if (meta == null || meta.optString("name").isEmpty() || meta.optString("libraryName").isEmpty()) {
            dest.delete()
            return "invalid"
        }
        if (!apiAllowed(meta)) {
            dest.delete()
            return "api"
        }
        return if (installZip(dest)) "ok" else "invalid"
    }

    fun removeDriver(fileName: String): Boolean {
        val zip = storedZip(fileName) ?: return false
        val meta = readMetaFromZip(zip)
        val installed = readMeta(File(installDir(), META_JSON))
        val removingActive = meta != null && installed != null && sameDriver(installed, meta)
        if (!zip.delete()) {
            return false
        }
        if (removingActive) {
            useSystemDriver()
        }
        return true
    }

    private fun installZip(zip: File): Boolean {
        clearInstallDir()
        return try {
            unzip(zip, installDir())
            if (readMeta(File(installDir(), META_JSON))?.optString("libraryName").isNullOrEmpty()) {
                clearInstallDir()
                initializeDriverParameters()
                false
            } else {
                initializeDriverParameters()
                true
            }
        } catch (_: Exception) {
            clearInstallDir()
            initializeDriverParameters()
            false
        }
    }

    private fun clearInstallDir() {
        val dir = installDir()
        dir.deleteRecursively()
        dir.mkdirs()
    }

    private fun unzip(zip: File, dest: File) {
        val destRoot = dest.canonicalFile
        ZipInputStream(zip.inputStream().buffered()).use { zis ->
            var entry = zis.nextEntry
            while (entry != null) {
                if (!entry.isDirectory) {
                    val outFile = File(destRoot, entry.name).canonicalFile
                    val rootPath = destRoot.path + File.separator
                    if (outFile.path != destRoot.path && !outFile.path.startsWith(rootPath)) {
                        throw SecurityException("zip slip")
                    }
                    outFile.parentFile?.mkdirs()
                    outFile.outputStream().use { output -> zis.copyTo(output) }
                }
                zis.closeEntry()
                entry = zis.nextEntry
            }
        }
    }

    private fun installDir(): File {
        return File(NxEmuApplication.appContext.filesDir, "gpu_driver")
    }

    private fun storageDir(): File {
        return File(NxEmuApplication.appContext.filesDir, "gpu_drivers")
    }

    private fun storedZip(fileName: String): File? {
        val name = File(fileName).name
        if (name != fileName || name.contains("..")) {
            return null
        }
        val zip = File(storageDir(), name)
        return if (zip.isFile) zip else null
    }

    private fun apiAllowed(meta: JSONObject): Boolean {
        val minApi = meta.optInt("minApi", 0)
        return minApi <= Build.VERSION.SDK_INT
    }

    private fun sameDriver(installed: JSONObject, zipMeta: JSONObject): Boolean {
        if (installed.optString("name") != zipMeta.optString("name")) {
            return false
        }
        if (installed.optString("libraryName") != zipMeta.optString("libraryName")) {
            return false
        }
        val installedVersion = installed.optString("driverVersion")
        val zipVersion = zipMeta.optString("driverVersion")
        return installedVersion.isEmpty() || zipVersion.isEmpty() || installedVersion == zipVersion
    }

    private fun readMetaFromZip(zip: File): JSONObject? {
        if (!zip.isFile) {
            return null
        }
        return try {
            ZipFile(zip).use { archive ->
                val entries = archive.entries()
                while (entries.hasMoreElements()) {
                    val entry = entries.nextElement()
                    if (!entry.isDirectory && entry.name.endsWith(META_JSON, ignoreCase = true) && entry.size <= MAX_META_BYTES) {
                        archive.getInputStream(entry).use { input ->
                            return JSONObject(input.readBytes().toString(Charsets.UTF_8))
                        }
                    }
                }
                null
            }
        } catch (_: Exception) {
            null
        }
    }

    private fun readMeta(file: File): JSONObject? {
        if (!file.isFile || file.length() > MAX_META_BYTES) {
            return null
        }
        return try {
            JSONObject(file.readText())
        } catch (_: Exception) {
            null
        }
    }

    private fun displayName(context: Context, uri: Uri): String {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) {
                val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (index >= 0) {
                    return cursor.getString(index) ?: "driver.zip"
                }
            }
        }
        return "driver.zip"
    }

    private fun safeZipName(raw: String): String {
        val base = raw.substringAfterLast('/').substringAfterLast('\\').ifBlank { "driver.zip" }
        val cleaned = base.replace(Regex("[^A-Za-z0-9._-]"), "_")
        return if (cleaned.endsWith(".zip", ignoreCase = true)) cleaned else "$cleaned.zip"
    }
}
