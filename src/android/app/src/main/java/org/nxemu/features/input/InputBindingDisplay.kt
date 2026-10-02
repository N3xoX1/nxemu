package org.nxemu.features.input

import org.nxemu.NxEmuApplication
import org.nxemu.R
import org.nxemu.features.input.model.ButtonName
import org.nxemu.utils.ParamPackage

object InputBindingDisplay {
    fun buttonToText(param: ParamPackage): String {
        val context = NxEmuApplication.appContext
        if (!param.has("engine")) {
            return context.getString(R.string.not_set)
        }

        val toggle = if (param.get("toggle", false)) "~" else ""
        val inverted = if (param.get("inverted", false)) "!" else ""
        val invert = if (param.get("invert", "+") == "-") "-" else ""
        val turbo = if (param.get("turbo", false)) "$" else ""
        val commonButtonName = NativeInput.getButtonName(param)

        if (commonButtonName == ButtonName.Invalid) {
            return context.getString(R.string.invalid)
        }
        if (commonButtonName == ButtonName.Engine) {
            return param.get("engine", "")
        }
        if (commonButtonName == ButtonName.Value) {
            if (param.has("hat")) {
                val hat = directionName(param.get("direction", ""))
                return context.getString(R.string.qualified_hat, turbo, toggle, inverted, hat)
            }
            if (param.has("axis")) {
                val axis = param.get("axis", "")
                return context.getString(
                    R.string.qualified_button_stick_axis,
                    toggle,
                    inverted,
                    invert,
                    axis,
                )
            }
            if (param.has("button")) {
                val button = param.get("button", "")
                return context.getString(R.string.qualified_button, turbo, toggle, inverted, button)
            }
        }
        return context.getString(R.string.unknown)
    }

    fun analogToText(param: ParamPackage, direction: String): String {
        val context = NxEmuApplication.appContext
        if (!param.has("engine")) {
            return context.getString(R.string.not_set)
        }
        if (param.get("engine", "") == "analog_from_button") {
            return buttonToText(ParamPackage(param.get(direction, "")))
        }
        if (!param.has("axis_x") || !param.has("axis_y")) {
            return context.getString(R.string.unknown)
        }

        val xAxis = param.get("axis_x", "")
        val yAxis = param.get("axis_y", "")
        val xInvert = param.get("invert_x", "+") == "-"
        val yInvert = param.get("invert_y", "+") == "-"

        if (direction == "modifier") {
            return context.getString(R.string.unused)
        }

        return when (direction) {
            "up" -> context.getString(R.string.qualified_axis, yAxis, if (yInvert) "+" else "-")
            "down" -> context.getString(R.string.qualified_axis, yAxis, if (yInvert) "-" else "+")
            "left" -> context.getString(R.string.qualified_axis, xAxis, if (xInvert) "+" else "-")
            "right" -> context.getString(R.string.qualified_axis, xAxis, if (xInvert) "-" else "+")
            else -> context.getString(R.string.unknown)
        }
    }

    fun getDisplayString(params: ParamPackage, control: String): String {
        val deviceName = params.get("display", "")
        if (deviceName.isEmpty()) {
            return NxEmuApplication.appContext.getString(R.string.not_set)
        }
        return "$deviceName: $control"
    }

    private fun directionName(direction: String): String {
        val context = NxEmuApplication.appContext
        return when (direction) {
            "up" -> context.getString(R.string.up)
            "down" -> context.getString(R.string.down)
            "left" -> context.getString(R.string.left)
            "right" -> context.getString(R.string.right)
            else -> direction
        }
    }
}
