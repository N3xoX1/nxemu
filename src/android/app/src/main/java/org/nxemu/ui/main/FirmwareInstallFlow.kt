package org.nxemu.ui.main

import android.app.Activity
import android.app.AlertDialog
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import org.nxemu.NativeLibrary
import org.nxemu.utils.Translation.tr
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

class FirmwareInstallFlow(
    private val activity: Activity,
    private val onInstalled: () -> Unit,
) {
    private var dialog: AlertDialog? = null
    private var stageText: TextView? = null
    private var detailText: TextView? = null
    private var progressBar: ProgressBar? = null

    @Volatile
    private var cancelled = false

    var busy = false
        private set

    fun dismiss() {
        cancelled = true
        dialog?.dismiss()
        dialog = null
    }

    fun offerBeforeLaunch(path: String, launch: () -> Unit, cancel: () -> Unit) {
        busy = true
        Thread({
            val version = NativeLibrary.getGameCardFirmwareUpdate(path)
            val installed = if (version.isEmpty()) "" else installedVersion()
            activity.runOnUiThread {
                busy = false
                if (activity.isDestroyed) {
                    return@runOnUiThread
                }
                if (version.isEmpty()) {
                    launch()
                    return@runOnUiThread
                }
                val text = if (installed.isEmpty()) {
                    tr("No firmware is installed yet. NXEmu can install the game's before starting it.")
                } else {
                    tr("Firmware %1 is installed, and the game was made for a more recent one. NXEmu can install the game's before starting it.", installed)
                }
                show(
                    AlertDialog.Builder(activity)
                        .setTitle(tr("This game carries firmware %1", version))
                        .setMessage(text)
                        .setPositiveButton(tr("Install")) { _, _ -> install(path, launch, cancel) }
                        .setNeutralButton(tr("Start without")) { _, _ -> launch() }
                        .setNegativeButton(tr("Cancel")) { _, _ -> cancel() }
                        .setOnCancelListener { cancel() }
                )
            }
        }, "FirmwareCheck").start()
    }

    fun install(path: String, launch: (() -> Unit)? = null, cancel: (() -> Unit)? = null) {
        if (busy) {
            return
        }
        busy = true
        cancelled = false
        showProgress()
        Thread({
            val status = NativeLibrary.installFirmware(path, Listener())
            val installed = installedVersion()
            activity.runOnUiThread {
                busy = false
                dialog?.dismiss()
                dialog = null
                if (activity.isDestroyed) {
                    return@runOnUiThread
                }
                if (status == STATUS_INSTALLED) {
                    onInstalled()
                }
                showResult(status, installed, launch, cancel)
            }
        }, "FirmwareInstall").start()
    }

    private fun showProgress() {
        val padding = dp(24)
        val stage = TextView(activity).apply { text = tr("Copying the firmware...") }
        val bar = ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal).apply {
            isIndeterminate = true
            max = 1000
        }
        val detail = TextView(activity).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            visibility = View.GONE
        }
        val content = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, dp(16), padding, 0)
            addView(stage)
            addView(bar)
            addView(detail)
        }
        stageText = stage
        progressBar = bar
        detailText = detail
        show(
            AlertDialog.Builder(activity)
                .setTitle(tr("Installing firmware"))
                .setView(content)
                .setCancelable(false)
                .setNegativeButton(tr("Cancel")) { _, _ -> cancelled = true }
        )
        // Keep the dialog up after Cancel until the install thread notices and ends.
        dialog?.getButton(AlertDialog.BUTTON_NEGATIVE)?.setOnClickListener { view ->
            cancelled = true
            view.isEnabled = false
            stageText?.text = tr("Cancelling...")
        }
    }

    private fun showResult(status: Int, installed: String, launch: (() -> Unit)?, cancel: (() -> Unit)?) {
        val (title, text) = outcome(status, installed)
        val builder = AlertDialog.Builder(activity).setTitle(title).setMessage(text)
        if (launch == null) {
            builder.setPositiveButton(tr("OK"), null)
        } else if (status == STATUS_INSTALLED) {
            // The user already chose to start the game; this only confirms the install.
            builder.setPositiveButton(tr("OK")) { _, _ -> launch() }
                .setCancelable(false)
        } else {
            builder.setPositiveButton(tr("Start anyway")) { _, _ -> launch() }
                .setNegativeButton(tr("Cancel")) { _, _ -> cancel?.invoke() }
                .setOnCancelListener { cancel?.invoke() }
        }
        show(builder)
    }

    private fun outcome(status: Int, installed: String): Pair<String, String> = when (status) {
        STATUS_INSTALLED -> Pair(
            if (installed.isEmpty()) tr("Firmware installed") else tr("Firmware %1 installed", installed),
            tr("Games can use it from now on."),
        )
        STATUS_CANCELLED -> Pair(tr("Installation cancelled"), tr("The installed firmware was left as it was."))
        STATUS_SOURCE_UNREADABLE -> Pair(
            tr("The source could not be read"),
            tr("Check that the file is still there and that NXEmu may read it."),
        )
        STATUS_NOT_FIRMWARE -> Pair(
            tr("No firmware found there"),
            tr("Choose a firmware .zip archive or a .dxci game card image that carries an update."),
        )
        STATUS_STORAGE_UNAVAILABLE -> Pair(
            tr("The console's storage cannot be written"),
            tr("Check the free space on this device, then try again."),
        )
        STATUS_REPLACE_FAILED -> Pair(
            tr("The firmware could not be replaced"),
            tr("The installed firmware was put back. The log tells what went wrong."),
        )
        else -> Pair(
            tr("Copying the firmware failed"),
            tr("The installed firmware was left as it was. Check the free space; the log tells what went wrong."),
        )
    }

    private fun show(builder: AlertDialog.Builder) {
        dialog?.dismiss()
        dialog = centered(builder.show())
    }

    private fun centered(shown: AlertDialog): AlertDialog {
        shown.window?.setGravity(Gravity.CENTER)
        return shown
    }

    private fun installedVersion(): String =
        NativeLibrary.getFirmwareVersion().let { if (it == "N/A") "" else it }

    private fun dp(value: Int): Int = (value * activity.resources.displayMetrics.density).toInt()

    private inner class Listener : NativeLibrary.FirmwareInstallListener {
        private var shownStage = -1
        private var shownPermille = -1

        override fun onProgress(stage: Int, done: Long, total: Long): Boolean {
            val permille = if (total > 0) (done * 1000 / total).toInt() else -1
            if (stage != shownStage || permille != shownPermille) {
                shownStage = stage
                shownPermille = permille
                activity.runOnUiThread { showProgress(stage, done, total) }
            }
            return !cancelled
        }

        override fun confirmReplace(packageVersion: String, installedVersion: String): Boolean {
            if (cancelled) {
                return false
            }
            var replace = false
            val answered = CountDownLatch(1)
            activity.runOnUiThread {
                if (activity.isDestroyed) {
                    answered.countDown()
                    return@runOnUiThread
                }
                AlertDialog.Builder(activity)
                    .setTitle(tr("Replace the installed firmware?"))
                    .setMessage(tr("The package holds firmware %1, and firmware %2 is installed. Installing the package replaces it.", packageVersion, installedVersion))
                    .setPositiveButton(tr("Replace")) { _, _ -> replace = true }
                    .setNegativeButton(tr("Keep")) { _, _ -> }
                    .setOnDismissListener { answered.countDown() }
                    .show()
                    .let { centered(it) }
            }
            // The native install waits here, so give up if the activity goes away without an answer.
            while (!answered.await(200, TimeUnit.MILLISECONDS)) {
                if (cancelled) {
                    return false
                }
            }
            return replace && !cancelled
        }
    }

    private fun showProgress(stage: Int, done: Long, total: Long) {
        val bar = progressBar ?: return
        if (cancelled) {
            return
        }
        stageText?.text = when (stage) {
            STAGE_COPYING -> tr("Copying the firmware...")
            STAGE_CHECKING -> tr("Checking the firmware...")
            else -> tr("Putting the firmware in place...")
        }
        val detail = when {
            total <= 0 -> ""
            stage == STAGE_COPYING -> tr("%1 of %2", megabytes(done), megabytes(total))
            stage == STAGE_CHECKING -> tr("File %1 of %2", (done + 1).toString(), total.toString())
            else -> ""
        }
        detailText?.text = detail
        detailText?.visibility = if (detail.isEmpty()) View.GONE else View.VISIBLE
        bar.isIndeterminate = total <= 0
        if (total > 0) {
            bar.progress = (done * 1000 / total).toInt()
        }
    }

    private fun megabytes(bytes: Long): String = tr("%1 MB", "%.0f".format(bytes / (1024.0 * 1024.0)))

    companion object {
        // FirmwareInstallStage and FirmwareInstallStatus in nxemu-module-spec/system_loader.h
        private const val STAGE_COPYING = 0
        private const val STAGE_CHECKING = 1

        private const val STATUS_INSTALLED = 0
        private const val STATUS_CANCELLED = 1
        private const val STATUS_SOURCE_UNREADABLE = 2
        private const val STATUS_NOT_FIRMWARE = 3
        private const val STATUS_STORAGE_UNAVAILABLE = 4
        private const val STATUS_REPLACE_FAILED = 6
    }
}
