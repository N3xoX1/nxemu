package org.nxemu

import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import org.json.JSONArray
import java.net.URLDecoder
import java.util.concurrent.CopyOnWriteArrayList

object NativeLibrary {
    init {
        System.loadLibrary("nxemu-android")
    }

    private val settingChangedListeners = CopyOnWriteArrayList<(String) -> Unit>()

    fun addSettingChangedListener(listener: (String) -> Unit) {
        settingChangedListeners.add(listener)
    }

    fun removeSettingChangedListener(listener: (String) -> Unit) {
        settingChangedListeners.remove(listener)
    }

    @JvmStatic
    fun onSettingChanged(setting: String) {
        Log.d("NxEmu", "onSettingChanged (kotlin): $setting")
        settingChangedListeners.forEach { listener -> listener(setting) }
    }

    external fun appInit(appDirectory: String, nativeModuleLibDir: String)
    external fun appCleanup()

    external fun getSettingString(setting: String): String
    external fun getSettingBool(setting: String): Boolean
    external fun setSettingString(setting: String, value: String)
    external fun saveSettings()

    external fun queryRomMetadata(path: String): String
    external fun queryRomInfo(path: String): String

    external fun emulationSurfaceReady(surface: android.view.Surface, pixelRatio: Float, romPath: String): Boolean
    external fun emulationSurfaceDestroyed()
    external fun surfaceChanged(surface: android.view.Surface)

    /** Used from native [yuzu_common/fs/fs_android.cpp] via RegisterCallbacks. */
    @JvmStatic
    fun getParentDirectory(path: String): String {
        if (path.isEmpty()) return ""
        if (path.startsWith("content://")) {
            val name = getFilename(path)
            if (name.isEmpty()) return ""
            val parent = path.removeSuffix(name).trimEnd('/')
            return if (parent.endsWith("/document")) path else parent
        }
        return java.io.File(path).parent ?: ""
    }

    @JvmStatic
    fun getFilename(path: String): String {
        if (path.isEmpty()) return ""
        if (path.startsWith("content://")) {
            return DocumentFile.fromSingleUri(NxEmuApplication.appContext, Uri.parse(path))?.name
                ?: path.substringAfterLast('/', "")
        }
        return java.io.File(path).name
    }

    @JvmStatic
    fun getSize(path: String): Long {
        if (path.startsWith("content://")) {
            return DocumentFile.fromSingleUri(NxEmuApplication.appContext, Uri.parse(path))?.length()
                ?: 0L
        }
        return try {
            java.io.File(path).length()
        } catch (_: Exception) {
            0L
        }
    }

    @JvmStatic
    fun isDirectory(path: String): Boolean {
        if (path.startsWith("content://")) {
            return DocumentFile.fromSingleUri(NxEmuApplication.appContext, Uri.parse(path))
                ?.isDirectory ?: false
        }
        return try {
            java.io.File(path).isDirectory
        } catch (_: Exception) {
            false
        }
    }

    @JvmStatic
    fun exists(path: String): Boolean {
        if (path.startsWith("content://")) {
            return DocumentFile.fromSingleUri(NxEmuApplication.appContext, Uri.parse(path))
                ?.exists() ?: false
        }
        return try {
            java.io.File(path).exists()
        } catch (_: Exception) {
            false
        }
    }

    @JvmStatic
    fun openContentUri(path: String, mode: String): Int {
        if (!path.startsWith("content://")) {
            Log.e("NxEmu", "openContentUri: not a content URI")
            return -1
        }

        val readMode = if (mode == "r") "r" else "r"
        val context = NxEmuApplication.appContext
        val cr = context.contentResolver
        val candidates = buildSafOpenCandidates(path)

        for (uri in candidates) {
            try {
                val doc = DocumentFile.fromSingleUri(context, uri)
                if (doc != null && !doc.isDirectory) {
                    val openUri = doc.uri
                    cr.openFileDescriptor(openUri, readMode)?.let { pfd ->
                        val fd = pfd.detachFd()
                        pfd.close()
                        Log.d("NxEmu", "openContentUri ok (doc) fd=$fd uri=$openUri")
                        return fd
                    }
                }

                cr.openFileDescriptor(uri, readMode)?.let { pfd ->
                    val fd = pfd.detachFd()
                    pfd.close()
                    Log.d("NxEmu", "openContentUri ok fd=$fd uri=$uri")
                    return fd
                }

                cr.openAssetFileDescriptor(uri, readMode)?.let { afd ->
                    try {
                        val pfd = afd.parcelFileDescriptor
                        if (pfd != null) {
                            val fd = pfd.detachFd()
                            pfd.close()
                            Log.d("NxEmu", "openContentUri ok (afd) fd=$fd uri=$uri")
                            return fd
                        }
                    } finally {
                        afd.close()
                    }
                }
            } catch (e: Exception) {
                Log.w("NxEmu", "openContentUri candidate failed uri=$uri", e)
            }
        }

        Log.e(
            "NxEmu",
            "openContentUri failed for $path persisted=${cr.persistedUriPermissions.size}"
        )
        for (perm in cr.persistedUriPermissions) {
            Log.e("NxEmu", "  persisted uri=${perm.uri} read=${perm.isReadPermission}")
        }
        return -1
    }

    /** Some devices/providers reject combined tree/document URIs unless rebuilt. */
    private fun buildSafOpenCandidates(path: String): List<Uri> {
        val out = LinkedHashSet<Uri>()
        val original = Uri.parse(path)
        out.add(original)

        DocumentFile.fromSingleUri(NxEmuApplication.appContext, original)?.uri?.let { out.add(it) }

        val documentMarker = "/document/"
        if (path.contains(documentMarker)) {
            val treePart = path.substringBefore(documentMarker)
            val encodedDocId = path.substringAfter(documentMarker)
            if (treePart.contains("/tree/")) {
                try {
                    val treeUri = Uri.parse(treePart)
                    out.add(treeUri)
                    val docId = URLDecoder.decode(encodedDocId, Charsets.UTF_8.name())
                    out.add(DocumentsContract.buildDocumentUriUsingTree(treeUri, docId))
                    out.add(DocumentsContract.buildDocumentUri(treeUri.authority, docId))
                } catch (_: Exception) {
                }
            }
        }

        return out.toList()
    }

    @JvmStatic
    fun restorePersistedGameDirectoryAccess() {
        val persisted = NxEmuApplication.appContext.contentResolver.persistedUriPermissions
            .map { it.uri.toString() }
            .toSet()

        val existing = getSettingString(NXUISetting.GameDirectories)
        val dirs = try {
            JSONArray(existing)
        } catch (_: Exception) {
            JSONArray()
        }
        val kept = JSONArray()
        var changed = false
        for (i in 0 until dirs.length()) {
            val path = dirs.getString(i)
            val isContent = path.startsWith("content:", ignoreCase = true)
            if (!isContent || persisted.contains(path)) {
                kept.put(path)
            } else {
                changed = true
                Log.d("NxEmu", "Dropped game directory without persisted access: $path")
            }
        }
        if (changed) {
            setSettingString(NXUISetting.GameDirectories, kept.toString())
            saveSettings()
        }
    }

}
