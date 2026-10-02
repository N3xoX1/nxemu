// SPDX-FileCopyrightText: 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.nxemu.features.input.model

enum class InputType(val int: Int) {
    None(0),
    Button(1),
    Stick(2),
    Motion(3),
    Touch(4),
}
