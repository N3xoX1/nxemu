package org.nxemu

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import org.json.JSONArray
import java.io.File

/**
 * Collects ROM file paths/URIs from configured game directories (filesystem paths and SAF trees).
 */
object GameLibraryScanner {
    private val ROM_EXTENSIONS =
        setOf("nro", "dxci", "dnsp")

    fun scanRomUris(context: Context): String {
        val dirsJson = NativeLibrary.getSettingString(NXUISetting.GameDirectories)
        val dirs = try {
            JSONArray(dirsJson)
        } catch (_: Exception) {
            JSONArray()
        }
        val out = JSONArray()
        for (i in 0 until dirs.length()) {
            val entry = dirs.optString(i, "") ?: continue
            when {
                entry.startsWith("content://") -> {
                    val uri = Uri.parse(entry)
                    scanContentTree(context, uri, out)
                }
                entry.startsWith("file://") -> {
                    Uri.parse(entry).path?.let { scanFileTree(File(it), out) }
                }
                entry.startsWith("/") -> {
                    scanFileTree(File(entry), out)
                }
            }
        }
        return out.toString()
    }

    private fun scanContentTree(context: Context, treeUri: Uri, out: JSONArray) {
        val root = DocumentFile.fromTreeUri(context, treeUri) ?: return
        walkDocument(root, out)
    }

    private fun walkDocument(doc: DocumentFile, out: JSONArray) {
        if (!doc.isDirectory) return
        val children = doc.listFiles()
        for (child in children) {
            when {
                child.isDirectory -> walkDocument(child, out)
                child.isFile && isRomName(child.name) -> out.put(child.uri.toString())
            }
        }
    }

    private fun scanFileTree(root: File, out: JSONArray) {
        if (!root.exists()) return
        if (root.isFile) {
            if (isRomName(root.name)) {
                out.put(root.absolutePath)
            }
            return
        }
        root.walkTopDown()
            .maxDepth(Int.MAX_VALUE)
            .filter { it.isFile && isRomName(it.name) }
            .forEach { out.put(it.absolutePath) }
    }

    private fun isRomName(name: String?): Boolean {
        if (name.isNullOrEmpty()) return false
        val ext = name.substringAfterLast('.', "").lowercase()
        return ext in ROM_EXTENSIONS
    }
}
