// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>

#include "applets/software_keyboard.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/string_util.h"

void DefaultSoftwareKeyboardApplet::Close() {}

void DefaultSoftwareKeyboardApplet::InitializeKeyboard(
    bool is_inline, const KeyboardInitializeHostParameters * initialize_parameters, void * user_data_normal, SwkbdSubmitNormalFn submit_normal, void * user_data_inline, SwkbdSubmitInlineFn submit_inline)
{
    user_data_normal_ = user_data_normal;
    submit_normal_ = submit_normal;
    user_data_inline_ = user_data_inline;
    submit_inline_ = submit_inline;

    if (is_inline)
    {
        LOG_WARNING(Service_AM, "(STUBBED) called, backend requested to initialize the inline software keyboard.");
    }
    else
    {
        LOG_WARNING(Service_AM, "(STUBBED) called, backend requested to initialize the normal software keyboard.");
    }

    if (initialize_parameters)
    {
        const auto u16view = [](const uint16_t * p, uint32_t n) {
            if (!p || n == 0)
            {
                return std::u16string{};
            }
            return std::u16string(reinterpret_cast<const char16_t *>(p), n);
        };

        LOG_INFO(Service_AM, "\nKeyboardInitializeHostParameters:\nok_text={}\nheader_text={}\nsub_text={}\nguide_text={}\ninitial_text={}\nmax_text_length={}\nmin_text_length={}\ninitial_cursor_position={}\ntype={}\npassword_mode={}\ntext_draw_type={}\nkey_disable_flags={}\nuse_blur_background={}\nenable_backspace_button={}\nenable_return_button={}\ndisable_cancel_button={}", Common::UTF16ToUTF8(u16view(initialize_parameters->ok_text, initialize_parameters->ok_text_unit_count)), Common::UTF16ToUTF8(u16view(initialize_parameters->header_text, initialize_parameters->header_text_unit_count)), Common::UTF16ToUTF8(u16view(initialize_parameters->sub_text, initialize_parameters->sub_text_unit_count)), Common::UTF16ToUTF8(u16view(initialize_parameters->guide_text, initialize_parameters->guide_text_unit_count)), Common::UTF16ToUTF8(u16view(initialize_parameters->initial_text, initialize_parameters->initial_text_unit_count)), initialize_parameters->max_text_length, initialize_parameters->min_text_length, initialize_parameters->initial_cursor_position, static_cast<int>(initialize_parameters->type), static_cast<int>(initialize_parameters->password_mode), static_cast<int>(initialize_parameters->text_draw_type), initialize_parameters->key_disable_flags_raw, initialize_parameters->use_blur_background, initialize_parameters->enable_backspace_button, initialize_parameters->enable_return_button, initialize_parameters->disable_cancel_button);
    }
}

void DefaultSoftwareKeyboardApplet::ShowNormalKeyboard() const
{
    LOG_WARNING(Service_AM, "(STUBBED) called, backend requested to show the normal software keyboard.");

    SubmitNormalText(u"nxemu");
}

void DefaultSoftwareKeyboardApplet::ShowTextCheckDialog(uint32_t text_check_result_raw, const uint16_t * message_utf16, uint32_t message_utf16_unit_count) const
{
    LOG_WARNING(Service_AM, "(STUBBED) called, backend requested to show the text check dialog (result_raw={}).", text_check_result_raw);
    (void)message_utf16;
    (void)message_utf16_unit_count;
}

void DefaultSoftwareKeyboardApplet::ShowInlineKeyboard(const InlineAppearHostParameters * appear_parameters) const
{
    LOG_WARNING(Service_AM, "(STUBBED) called, backend requested to show the inline software keyboard.");

    if (!appear_parameters)
    {
        return;
    }

    LOG_INFO(Service_AM, "\nInlineAppearHostParameters:\nmax_text_length={}\nmin_text_length={}\nkey_top_scale_x={}\nkey_top_scale_y={}\nkey_top_translate_x={}\nkey_top_translate_y={}\ntype={}\nkey_disable_flags={}\nkey_top_as_floating={}\nenable_backspace_button={}\nenable_return_button={}\ndisable_cancel_button={}", appear_parameters->max_text_length, appear_parameters->min_text_length, appear_parameters->key_top_scale_x, appear_parameters->key_top_scale_y, appear_parameters->key_top_translate_x, appear_parameters->key_top_translate_y, static_cast<int>(appear_parameters->type), appear_parameters->key_disable_flags_raw, appear_parameters->key_top_as_floating, appear_parameters->enable_backspace_button, appear_parameters->enable_return_button, appear_parameters->disable_cancel_button);

    std::thread([this] { SubmitInlineText(u"yuzu"); }).detach();
}

void DefaultSoftwareKeyboardApplet::HideInlineKeyboard() const
{
    LOG_WARNING(Service_AM, "(STUBBED) called, backend requested to hide the inline software keyboard.");
}

void DefaultSoftwareKeyboardApplet::InlineTextChanged(const InlineTextHostParameters * text_parameters) const
{
    LOG_WARNING(Service_AM, "(STUBBED) called, backend requested to change the inline keyboard text.");

    if (!text_parameters || !submit_inline_)
    {
        return;
    }

    LOG_INFO(Service_AM, "\nInlineTextHostParameters:\ninput_text={}\ncursor_position={}", Common::UTF16ToUTF8(std::u16string(reinterpret_cast<const char16_t *>(text_parameters->input_text), text_parameters->input_text_unit_count)), text_parameters->cursor_position);

    submit_inline_(user_data_inline_, static_cast<uint32_t>(Service::AM::Frontend::SwkbdReplyType::ChangedString), text_parameters->input_text, text_parameters->input_text_unit_count, text_parameters->cursor_position);
}

void DefaultSoftwareKeyboardApplet::ExitKeyboard() const
{
    LOG_WARNING(Service_AM, "(STUBBED) called, backend requested to exit the software keyboard.");
}

void DefaultSoftwareKeyboardApplet::SubmitNormalText(std::u16string text) const
{
    if (!submit_normal_)
    {
        return;
    }

    submit_normal_(user_data_normal_, static_cast<uint32_t>(Service::AM::Frontend::SwkbdResult::Ok), reinterpret_cast<const uint16_t *>(text.data()), static_cast<uint32_t>(text.size()), true);
}

void DefaultSoftwareKeyboardApplet::SubmitInlineText(std::u16string_view text) const
{
    if (!submit_inline_)
    {
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    for (std::size_t index = 0; index < text.size(); ++index)
    {
        submit_inline_(user_data_inline_, static_cast<uint32_t>(Service::AM::Frontend::SwkbdReplyType::ChangedString), reinterpret_cast<const uint16_t *>(text.data()), static_cast<uint32_t>(index + 1), static_cast<s32>(index) + 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    submit_inline_(user_data_inline_, static_cast<uint32_t>(Service::AM::Frontend::SwkbdReplyType::DecidedEnter), reinterpret_cast<const uint16_t *>(text.data()), static_cast<uint32_t>(text.size()), static_cast<s32>(text.size()));
}