package org.nxemu.ui.main

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.webkit.WebView

class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(WebView(this).apply {
            setBackgroundColor(Color.BLACK)
            loadUrl("file:///android_asset/index.html")
        })
    }
}
