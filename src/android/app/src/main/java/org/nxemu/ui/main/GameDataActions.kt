package org.nxemu.ui.main

import android.app.AlertDialog
import android.net.Uri
import android.view.Gravity
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.documentfile.provider.DocumentFile
import org.nxemu.utils.Translation.tr
import java.io.File
import java.io.IOException
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

class GameDataActions(
    private val activity: MainActivity,
    private val onChanged: () -> Unit,
) {
    private var modDirectory = ""
    private var saveDirectory = ""

    private val pickModFolder = activity.registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        uri?.let { copyMod(it) }
    }

    private val createSaveZip = activity.registerForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        uri?.let { writeSaveZip(it) }
    }

    private val pickSaveZip = activity.registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri?.let { readSaveZip(it) }
    }

    fun installMod(directory: String) {
        modDirectory = directory
        pickModFolder.launch(null)
    }

    fun exportSave(directory: String, fileName: String) {
        saveDirectory = directory
        createSaveZip.launch(fileName.replace(Regex("[\\\\/:*?\"<>|]"), " "))
    }

    fun importSave(directory: String) {
        saveDirectory = directory
        pickSaveZip.launch(arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
    }

    private fun copyMod(uri: Uri) {
        val folder = DocumentFile.fromTreeUri(activity, uri)
        val parts = setOf("romfs", "exefs", "cheats")
        if (folder == null || folder.listFiles().none { it.isDirectory && it.name?.lowercase() in parts }) {
            message(tr("Not a mod folder"), tr("Choose the mod's own folder: the one that holds a romfs, exefs or cheats folder."))
            return
        }
        val target = File(modDirectory, folder.name ?: "Mod")
        work(tr("Installing the mod..."), done = folder.name?.let { tr("%1 is installed.", it) } ?: tr("The mod is installed.")) {
            target.deleteRecursively()
            copyTree(folder, target)
        }
    }

    private fun copyTree(source: DocumentFile, target: File) {
        if (!target.mkdirs() && !target.isDirectory) {
            throw IOException(tr("Could not create %1", target.path))
        }
        for (child in source.listFiles()) {
            val name = child.name ?: continue
            if (child.isDirectory) {
                copyTree(child, File(target, name))
            } else {
                activity.contentResolver.openInputStream(child.uri)?.use { input ->
                    File(target, name).outputStream().use { input.copyTo(it) }
                } ?: throw IOException(tr("Could not read %1", name))
            }
        }
    }

    private fun writeSaveZip(uri: Uri) {
        val source = File(saveDirectory)
        work(tr("Exporting the save data..."), done = tr("The save data is exported.")) {
            activity.contentResolver.openOutputStream(uri)?.use { output ->
                ZipOutputStream(output).use { zip ->
                    source.walkTopDown().filter { it.isFile }.forEach { file ->
                        zip.putNextEntry(ZipEntry(file.relativeTo(source).invariantSeparatorsPath))
                        file.inputStream().use { it.copyTo(zip) }
                        zip.closeEntry()
                    }
                }
            } ?: throw IOException(tr("Could not write the zip file"))
        }
    }

    // Extracts next to the save first, so a broken zip leaves the current save as it was.
    private fun readSaveZip(uri: Uri) {
        val target = File(saveDirectory)
        val staging = File(target.parentFile, ".${target.name}.import")
        work(tr("Importing the save data..."), done = tr("The save data is imported.")) {
            staging.deleteRecursively()
            staging.mkdirs()
            var files = 0
            activity.contentResolver.openInputStream(uri)?.use { input ->
                ZipInputStream(input).use { zip ->
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        val file = File(staging, entry.name)
                        if (!file.canonicalPath.startsWith(staging.canonicalPath + File.separator)) {
                            throw IOException(tr("The zip file has a path outside its folder"))
                        }
                        if (entry.isDirectory) {
                            file.mkdirs()
                        } else {
                            file.parentFile?.mkdirs()
                            file.outputStream().use { zip.copyTo(it) }
                            files++
                        }
                    }
                }
            } ?: throw IOException(tr("Could not read the zip file"))
            if (files == 0) {
                staging.deleteRecursively()
                throw IOException(tr("The zip file holds no save data"))
            }
            // Some tools wrap the save in a folder named after the title ID.
            val inner = staging.listFiles()?.singleOrNull()
            val contents = if (inner != null && inner.isDirectory && inner.name.equals(target.name, ignoreCase = true)) inner else staging
            target.deleteRecursively()
            target.parentFile?.mkdirs()
            if (!contents.renameTo(target)) {
                throw IOException(tr("Could not put the save data in place"))
            }
            staging.deleteRecursively()
        }
    }

    private fun work(title: String, done: String, task: () -> Unit) {
        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            val padding = (24 * resources.displayMetrics.density).toInt()
            setPadding(padding, padding, padding, padding)
            addView(ProgressBar(activity))
            addView(TextView(activity).apply {
                text = title
                setPadding(padding / 2, 0, 0, 0)
            })
        }
        val dialog = AlertDialog.Builder(activity).setView(content).setCancelable(false).show()
        dialog.window?.setGravity(Gravity.CENTER)
        Thread({
            val error = try {
                task()
                null
            } catch (e: Exception) {
                e.message ?: e.toString()
            }
            activity.runOnUiThread {
                dialog.dismiss()
                if (activity.isDestroyed) {
                    return@runOnUiThread
                }
                if (error == null) {
                    Toast.makeText(activity, done, Toast.LENGTH_LONG).show()
                } else {
                    message(tr("That did not work"), error)
                }
                onChanged()
            }
        }, "GameData").start()
    }

    private fun message(title: String, text: String) {
        AlertDialog.Builder(activity)
            .setTitle(title)
            .setMessage(text)
            .setPositiveButton(tr("OK"), null)
            .show()
            .window?.setGravity(Gravity.CENTER)
    }
}
