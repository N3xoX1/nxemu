package org.nxemu.features.settings.model

enum class BooleanSetting {
    JOYSTICK_REL_CENTER,
    DPAD_SLIDE,
    HAPTIC_FEEDBACK,
    SHOW_INPUT_OVERLAY,
    TOUCHSCREEN;

    fun getBoolean(needsGlobal: Boolean = false): Boolean = true
}
