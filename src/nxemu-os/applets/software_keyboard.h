// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "applets/applet.h"
#include "core/hle/service/am/frontend/applet_software_keyboard_types.h"
#include <nxemu-module-spec/operating_system.h>

struct InlineTextParameters
{
    std::u16string input_text;
    s32 cursor_position;
};

class DefaultSoftwareKeyboardApplet final :
    public ISoftwareKeyboardApplet
{
public:
    void Close() override;
    void InitializeKeyboard(bool is_inline, const KeyboardInitializeHostParameters * initialize_parameters, void * user_data_normal, SwkbdSubmitNormalFn submit_normal, void * user_data_inline, SwkbdSubmitInlineFn submit_inline) override;
    void ShowNormalKeyboard() const override;
    void ShowTextCheckDialog(uint32_t text_check_result_raw, const uint16_t * message_utf16, uint32_t message_utf16_unit_count) const override;
    void ShowInlineKeyboard(const InlineAppearHostParameters * appear_parameters) const override;
    void HideInlineKeyboard() const override;
    void InlineTextChanged(const InlineTextHostParameters * text_parameters) const override;
    void ExitKeyboard() const override;

private:
    void SubmitNormalText(std::u16string text) const;
    void SubmitInlineText(std::u16string_view text) const;

    void * user_data_normal_{};
    SwkbdSubmitNormalFn submit_normal_{};

    void * user_data_inline_{};
    SwkbdSubmitInlineFn submit_inline_{};
};