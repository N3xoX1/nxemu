package org.nxemu

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

object GameLibraryScanner {
    private val FILE_EXTENSIONS =
        setOf("nro", "dxci", "dnsp", "nca")

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
                    scanContentTree(context.contentResolver, uri, out)
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

    private fun scanContentTree(resolver: ContentResolver, treeUri: Uri, out: JSONArray) {
        try {
            walkTree(resolver, treeUri, DocumentsContract.getTreeDocumentId(treeUri), out)
        } catch (e: Exception) {
            Log.w("NxEmu", "Could not scan $treeUri", e)
        }
    }

    // One query per directory returns every child's name, type, size and date.
    private fun walkTree(resolver: ContentResolver, treeUri: Uri, parentId: String, out: JSONArray) {
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentId)
        val columns = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
        )
        resolver.query(children, columns, null, null, null)?.use { cursor ->
            while (cursor.moveToNext()) {
                val id = cursor.getString(0) ?: continue
                val name = cursor.getString(1)
                if (cursor.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR) {
                    walkTree(resolver, treeUri, id, out)
                } else if (isGameFileName(name)) {
                    val uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, id)
                    out.put(entry(uri.toString(), cursor.getLong(3), cursor.getLong(4)))
                }
            }
        }
    }

    private fun scanFileTree(root: File, out: JSONArray) {
        if (!root.exists()) return
        root.walkTopDown()
            .filter { it.isFile && isGameFileName(it.name) }
            .forEach { out.put(entry(it.absolutePath, it.length(), it.lastModified())) }
    }

    private fun entry(path: String, size: Long, modified: Long): JSONObject =
        JSONObject().put("path", path).put("size", size).put("modified", modified)

    private fun isGameFileName(name: String?): Boolean {
        if (name.isNullOrEmpty()) return false
        val ext = name.substringAfterLast('.', "").lowercase()
        return ext in FILE_EXTENSIONS
    }
}
