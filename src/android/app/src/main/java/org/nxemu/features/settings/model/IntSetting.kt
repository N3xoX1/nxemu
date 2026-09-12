package org.nxemu.features.settings.model

enum class IntSetting(private val default: Int) {
    OVERLAY_SCALE(50),
    OVERLAY_OPACITY(100);

    fun getInt(needsGlobal: Boolean = false): Int = default
}
