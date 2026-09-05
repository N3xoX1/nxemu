package org.nxemu.ui.main

import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.webkit.ConsoleMessage
import android.webkit.WebChromeClient
import android.webkit.WebView
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import org.json.JSONArray
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
            setBackgroundColor(Color.BLACK)
            settings.javaScriptEnabled = true
            webChromeClient = object : WebChromeClient() {
                override fun onConsoleMessage(msg: ConsoleMessage): Boolean {
                    Log.d("NxEmu-JS", "${msg.message()} [${msg.sourceId()}:${msg.lineNumber()}]")
                    return true
                }
            }
            addJavascriptInterface(NxEmuBridge(this@MainActivity), "NxEmu")
        }
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

    fun launchAddGameDirectory() {
        addGameDirectory.launch(null)
    }
}
