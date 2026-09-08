package org.nxemu.ui.main

import android.content.res.Configuration
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.webkit.ConsoleMessage
import android.webkit.JsResult
import android.webkit.WebChromeClient
import android.webkit.WebView
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import org.json.JSONArray
import org.json.JSONObject
import org.nxemu.NXUISetting
import org.nxemu.NativeLibrary

class MainActivity : ComponentActivity() {
    private lateinit var webView: WebView

    private val addGameDirectory = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? ->
        uri?.let {
            contentResolver.takePersistableUriPermission(
                it,
                android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION
            )
            Log.d("NxEmu", "Folder selected: $it")

            val path = it.toString()
            val existing = NativeLibrary.getSettingString(NXUISetting.GameDirectories)
            val dirs = try {
                JSONArray(existing)
            } catch (e: Exception) {
                JSONArray()
            }
            val paths = (0 until dirs.length()).map { i -> dirs.getString(i) }
            if (!paths.contains(path)) {
                dirs.put(path)
                NativeLibrary.setSettingString(NXUISetting.GameDirectories, dirs.toString())
                NativeLibrary.saveSettings()
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        webView = WebView(this).apply {
            setBackgroundColor(webViewBackground())
            settings.javaScriptEnabled = true
            webChromeClient = object : WebChromeClient() {
                override fun onConsoleMessage(msg: ConsoleMessage): Boolean {
                    Log.d("NxEmu-JS", "${msg.message()} [${msg.sourceId()}:${msg.lineNumber()}]")
                    return true
                }

                override fun onJsConfirm(
                    view: WebView?,
                    url: String?,
                    message: String?,
                    result: JsResult,
                ): Boolean {
                    android.app.AlertDialog.Builder(this@MainActivity)
                        .setMessage(message)
                        .setPositiveButton(android.R.string.ok) { _, _ -> result.confirm() }
                        .setNegativeButton(android.R.string.cancel) { _, _ -> result.cancel() }
                        .setOnCancelListener { result.cancel() }
                        .show()
                    return true
                }
            }
            addJavascriptInterface(NxEmuBridge(this@MainActivity), "NxEmu")
        }
        onBackPressedDispatcher.addCallback(
            this,
            object : OnBackPressedCallback(true) {
                override fun handleOnBackPressed() {
                    webView.evaluateJavascript("handleAndroidBack()") { result ->
                        if (result != "true" && result != "\"true\"") {
                            finish()
                        }
                    }
                }
            },
        )
        NativeLibrary.onSettingChangedListener = { setting ->
            runOnUiThread {
                webView.evaluateJavascript(
                    "onSettingChanged('${setting.replace("'", "\\'")}')",
                    null
                )
            }
        }
        webView.loadUrl("file:///android_asset/index.html")
        setContentView(webView)
    }

    override fun onDestroy() {
        NativeLibrary.onSettingChangedListener = null
        super.onDestroy()
    }

    fun AddGameDirectory() {
        addGameDirectory.launch(null)
    }

    fun dispatchGameLibraryPaths(gen: Int, json: String) {
        webView.evaluateJavascript(
            "onGameLibraryPaths($gen, ${JSONObject.quote(json)})",
            null,
        )
    }

    private fun webViewBackground(): Int {
        val night = resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK
        return if (night == Configuration.UI_MODE_NIGHT_YES) {
            Color.parseColor("#121212")
        } else {
            Color.WHITE
        }
    }
}
