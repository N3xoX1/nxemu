// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/am/frontend/applet_software_keyboard.h"
#include "applets/software_keyboard.h"
#include "core/core.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/service/storage.h"
#include "yuzu_common/string_util.h"

namespace Service::AM::Frontend
{

namespace
{

void CALL SoftwareKeyboardSubmitNormalThunk(void * user_data, uint32_t result_raw, const uint16_t * text_utf16, uint32_t text_utf16_unit_count, bool confirmed)
{
    auto * const self = static_cast<SoftwareKeyboard *>(user_data);
    self->SubmitTextNormal(static_cast<SwkbdResult>(result_raw), text_utf16 != nullptr && text_utf16_unit_count > 0 ? std::u16string(reinterpret_cast<const char16_t *>(text_utf16), text_utf16_unit_count) : std::u16string{}, confirmed);
}

void CALL SoftwareKeyboardSubmitInlineThunk(void * user_data, uint32_t reply_raw, const uint16_t * text_utf16, uint32_t text_utf16_unit_count, int32_t cursor)
{
    auto * const self = static_cast<SoftwareKeyboard *>(user_data);
    self->SubmitTextInline(static_cast<SwkbdReplyType>(reply_raw), text_utf16 != nullptr && text_utf16_unit_count > 0 ? std::u16string(reinterpret_cast<const char16_t *>(text_utf16), text_utf16_unit_count) : std::u16string{}, cursor);
}

static void CopyUtf16ToHostBuffer(uint16_t * buf, std::size_t buf_units, uint32_t * count_out, const std::u16string & s)
{
    if (buf_units == 0)
    {
        *count_out = 0;
        return;
    }
    const std::size_t max_copy = buf_units - 1;
    const std::size_t n = std::min(s.size(), max_copy);
    for (std::size_t i = 0; i < n; ++i)
    {
        buf[i] = static_cast<uint16_t>(s[i]);
    }
    buf[n] = 0;
    *count_out = static_cast<uint32_t>(n);
}

// The maximum number of UTF-16 characters that can be input into the swkbd text field.
constexpr u32 DEFAULT_MAX_TEXT_LENGTH = 500;

constexpr std::size_t REPLY_BASE_SIZE = sizeof(SwkbdState) + sizeof(SwkbdReplyType);
constexpr std::size_t REPLY_UTF8_SIZE = 0x7D4;
constexpr std::size_t REPLY_UTF16_SIZE = 0x3EC;

constexpr const char * GetTextCheckResultName(SwkbdTextCheckResult text_check_result)
{
    switch (text_check_result)
    {
    case SwkbdTextCheckResult::Success:
        return "Success";
    case SwkbdTextCheckResult::Failure:
        return "Failure";
    case SwkbdTextCheckResult::Confirm:
        return "Confirm";
    case SwkbdTextCheckResult::Silent:
        return "Silent";
    default:
        UNIMPLEMENTED_MSG("Unknown TextCheckResult={}", text_check_result);
        return "Unknown";
    }
}

void SetReplyBase(std::vector<u8> & reply, SwkbdState state, SwkbdReplyType reply_type)
{
    std::memcpy(reply.data(), &state, sizeof(SwkbdState));
    std::memcpy(reply.data() + sizeof(SwkbdState), &reply_type, sizeof(SwkbdReplyType));
}

} // Anonymous namespace

SoftwareKeyboard::SoftwareKeyboard(Core::System & system_, std::shared_ptr<Applet> applet_, LibraryAppletMode applet_mode_, ISoftwareKeyboardApplet & frontend_) :
    FrontendApplet{system_, applet_, applet_mode_},
    frontend{frontend_}
{
}

SoftwareKeyboard::~SoftwareKeyboard() = default;

void SoftwareKeyboard::Initialize()
{
    FrontendApplet::Initialize();

    LOG_INFO(Service_AM, "Initializing Software Keyboard Applet with LibraryAppletMode={}", applet_mode);
    LOG_DEBUG(Service_AM, "Initializing Applet with common_args: arg_version={}, lib_version={}, play_startup_sound={}, size={}, system_tick={}, theme_color={}", common_args.arguments_version, common_args.library_version, common_args.play_startup_sound, common_args.size, common_args.system_tick, common_args.theme_color);

    swkbd_applet_version = SwkbdAppletVersion{common_args.library_version};

    switch (applet_mode)
    {
    case LibraryAppletMode::AllForeground:
        InitializeForeground();
        break;
    case LibraryAppletMode::PartialForeground:
    case LibraryAppletMode::PartialForegroundIndirectDisplay:
        InitializePartialForeground(applet_mode);
        break;
    default:
        ASSERT_MSG(false, "Invalid LibraryAppletMode={}", applet_mode);
        break;
    }
}

Result SoftwareKeyboard::GetStatus() const
{
    return status;
}

void SoftwareKeyboard::ExecuteInteractive()
{
    if (complete)
    {
        return;
    }

    if (is_background)
    {
        ProcessInlineKeyboardRequest();
    }
    else
    {
        ProcessTextCheck();
    }
}

void SoftwareKeyboard::Execute()
{
    if (complete)
    {
        return;
    }

    if (is_background)
    {
        return;
    }

    ShowNormalKeyboard();
}

void SoftwareKeyboard::SubmitTextNormal(SwkbdResult result, std::u16string submitted_text, bool confirmed)
{
    if (complete)
    {
        return;
    }

    if (swkbd_config_common.use_text_check && result == SwkbdResult::Ok)
    {
        if (confirmed)
        {
            SubmitNormalOutputAndExit(result, submitted_text);
        }
        else
        {
            SubmitForTextCheck(submitted_text);
        }
    }
    else
    {
        SubmitNormalOutputAndExit(result, submitted_text);
    }
}

void SoftwareKeyboard::SubmitTextInline(SwkbdReplyType reply_type, std::u16string submitted_text, s32 cursor_position)
{
    if (complete)
    {
        return;
    }

    current_text = std::move(submitted_text);
    current_cursor_position = cursor_position;

    if (inline_use_utf8)
    {
        switch (reply_type)
        {
        case SwkbdReplyType::ChangedString:
            reply_type = SwkbdReplyType::ChangedStringUtf8;
            break;
        case SwkbdReplyType::MovedCursor:
            reply_type = SwkbdReplyType::MovedCursorUtf8;
            break;
        case SwkbdReplyType::DecidedEnter:
            reply_type = SwkbdReplyType::DecidedEnterUtf8;
            break;
        default:
            break;
        }
    }

    if (use_changed_string_v2)
    {
        switch (reply_type)
        {
        case SwkbdReplyType::ChangedString:
            reply_type = SwkbdReplyType::ChangedStringV2;
            break;
        case SwkbdReplyType::ChangedStringUtf8:
            reply_type = SwkbdReplyType::ChangedStringUtf8V2;
            break;
        default:
            break;
        }
    }

    if (use_moved_cursor_v2)
    {
        switch (reply_type)
        {
        case SwkbdReplyType::MovedCursor:
            reply_type = SwkbdReplyType::MovedCursorV2;
            break;
        case SwkbdReplyType::MovedCursorUtf8:
            reply_type = SwkbdReplyType::MovedCursorUtf8V2;
            break;
        default:
            break;
        }
    }

    SendReply(reply_type);
}

void SoftwareKeyboard::InitializeForeground()
{
    LOG_INFO(Service_AM, "Initializing Normal Software Keyboard Applet.");

    is_background = false;

    const auto swkbd_config_storage = PopInData();
    ASSERT(swkbd_config_storage != nullptr);

    const auto & swkbd_config_data = swkbd_config_storage->GetData();
    ASSERT(swkbd_config_data.size() >= sizeof(SwkbdConfigCommon));

    std::memcpy(&swkbd_config_common, swkbd_config_data.data(), sizeof(SwkbdConfigCommon));

    switch (swkbd_applet_version)
    {
    case SwkbdAppletVersion::Version5:
    case SwkbdAppletVersion::Version65542:
        ASSERT(swkbd_config_data.size() == sizeof(SwkbdConfigCommon) + sizeof(SwkbdConfigOld));
        std::memcpy(&swkbd_config_old, swkbd_config_data.data() + sizeof(SwkbdConfigCommon), sizeof(SwkbdConfigOld));
        break;
    case SwkbdAppletVersion::Version196615:
    case SwkbdAppletVersion::Version262152:
    case SwkbdAppletVersion::Version327689:
        ASSERT(swkbd_config_data.size() == sizeof(SwkbdConfigCommon) + sizeof(SwkbdConfigOld2));
        std::memcpy(&swkbd_config_old2, swkbd_config_data.data() + sizeof(SwkbdConfigCommon), sizeof(SwkbdConfigOld2));
        break;
    case SwkbdAppletVersion::Version393227:
    case SwkbdAppletVersion::Version524301:
        ASSERT(swkbd_config_data.size() == sizeof(SwkbdConfigCommon) + sizeof(SwkbdConfigNew));
        std::memcpy(&swkbd_config_new, swkbd_config_data.data() + sizeof(SwkbdConfigCommon), sizeof(SwkbdConfigNew));
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown SwkbdConfig revision={} with size={}", swkbd_applet_version, swkbd_config_data.size());
        ASSERT(swkbd_config_data.size() >= sizeof(SwkbdConfigCommon) + sizeof(SwkbdConfigNew));
        std::memcpy(&swkbd_config_new, swkbd_config_data.data() + sizeof(SwkbdConfigCommon), sizeof(SwkbdConfigNew));
        break;
    }

    const auto work_buffer_storage = PopInData();
    ASSERT(work_buffer_storage != nullptr);

    if (swkbd_config_common.initial_string_length == 0)
    {
        InitializeFrontendNormalKeyboard();
        return;
    }

    const auto & work_buffer = work_buffer_storage->GetData();

    std::vector<char16_t> initial_string(swkbd_config_common.initial_string_length);

    std::memcpy(initial_string.data(), work_buffer.data() + swkbd_config_common.initial_string_offset, swkbd_config_common.initial_string_length * sizeof(char16_t));

    initial_text = Common::UTF16StringFromFixedZeroTerminatedBuffer(initial_string.data(),
                                                                    initial_string.size());

    LOG_DEBUG(Service_AM, "\nInitial Text: {}", Common::UTF16ToUTF8(initial_text));

    InitializeFrontendNormalKeyboard();
}

void SoftwareKeyboard::InitializePartialForeground(LibraryAppletMode library_applet_mode)
{
    LOG_INFO(Service_AM, "Initializing Inline Software Keyboard Applet.");

    is_background = true;

    const auto swkbd_inline_initialize_arg_storage = PopInData();
    ASSERT(swkbd_inline_initialize_arg_storage != nullptr);

    const auto & swkbd_inline_initialize_arg = swkbd_inline_initialize_arg_storage->GetData();
    ASSERT(swkbd_inline_initialize_arg.size() == sizeof(SwkbdInitializeArg));

    std::memcpy(&swkbd_initialize_arg, swkbd_inline_initialize_arg.data(), swkbd_inline_initialize_arg.size());

    if (swkbd_initialize_arg.library_applet_mode_flag)
    {
        ASSERT(library_applet_mode == LibraryAppletMode::PartialForeground);
    }
    else
    {
        ASSERT(library_applet_mode == LibraryAppletMode::PartialForegroundIndirectDisplay);
    }
}

void SoftwareKeyboard::ProcessTextCheck()
{
    const auto text_check_storage = PopInteractiveInData();
    ASSERT(text_check_storage != nullptr);

    const auto & text_check_data = text_check_storage->GetData();
    ASSERT(text_check_data.size() == sizeof(SwkbdTextCheck));

    SwkbdTextCheck swkbd_text_check;

    std::memcpy(&swkbd_text_check, text_check_data.data(), sizeof(SwkbdTextCheck));

    std::u16string text_check_message = [this, &swkbd_text_check]() -> std::u16string {
        if (swkbd_text_check.text_check_result == SwkbdTextCheckResult::Failure ||
            swkbd_text_check.text_check_result == SwkbdTextCheckResult::Confirm)
        {
            return swkbd_config_common.use_utf8 ? Common::UTF8ToUTF16(Common::StringFromFixedZeroTerminatedBuffer(reinterpret_cast<const char *>(swkbd_text_check.text_check_message.data()), swkbd_text_check.text_check_message.size() * sizeof(char16_t))) : Common::UTF16StringFromFixedZeroTerminatedBuffer( swkbd_text_check.text_check_message.data(), swkbd_text_check.text_check_message.size());
        }
        else
        {
            return u"";
        }
    }();

    LOG_INFO(Service_AM, "\nTextCheckResult: {}\nTextCheckMessage: {}",
             GetTextCheckResultName(swkbd_text_check.text_check_result),
             Common::UTF16ToUTF8(text_check_message));

    switch (swkbd_text_check.text_check_result)
    {
    case SwkbdTextCheckResult::Success:
        SubmitNormalOutputAndExit(SwkbdResult::Ok, current_text);
        break;
    case SwkbdTextCheckResult::Failure:
        ShowTextCheckDialog(SwkbdTextCheckResult::Failure, std::move(text_check_message));
        break;
    case SwkbdTextCheckResult::Confirm:
        ShowTextCheckDialog(SwkbdTextCheckResult::Confirm, std::move(text_check_message));
        break;
    case SwkbdTextCheckResult::Silent:
    default:
        break;
    }
}

void SoftwareKeyboard::ProcessInlineKeyboardRequest()
{
    const auto request_data_storage = PopInteractiveInData();
    ASSERT(request_data_storage != nullptr);

    const auto & request_data = request_data_storage->GetData();
    ASSERT(request_data.size() >= sizeof(SwkbdRequestCommand));

    SwkbdRequestCommand request_command;

    std::memcpy(&request_command, request_data.data(), sizeof(SwkbdRequestCommand));

    switch (request_command)
    {
    case SwkbdRequestCommand::Finalize:
        RequestFinalize(request_data);
        break;
    case SwkbdRequestCommand::SetUserWordInfo:
        RequestSetUserWordInfo(request_data);
        break;
    case SwkbdRequestCommand::SetCustomizeDic:
        RequestSetCustomizeDic(request_data);
        break;
    case SwkbdRequestCommand::Calc:
        RequestCalc(request_data);
        break;
    case SwkbdRequestCommand::SetCustomizedDictionaries:
        RequestSetCustomizedDictionaries(request_data);
        break;
    case SwkbdRequestCommand::UnsetCustomizedDictionaries:
        RequestUnsetCustomizedDictionaries(request_data);
        break;
    case SwkbdRequestCommand::SetChangedStringV2Flag:
        RequestSetChangedStringV2Flag(request_data);
        break;
    case SwkbdRequestCommand::SetMovedCursorV2Flag:
        RequestSetMovedCursorV2Flag(request_data);
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown SwkbdRequestCommand={}", request_command);
        break;
    }
}

void SoftwareKeyboard::SubmitNormalOutputAndExit(SwkbdResult result,
                                                 std::u16string submitted_text)
{
    std::vector<u8> out_data(sizeof(SwkbdResult) + STRING_BUFFER_SIZE);

    if (swkbd_config_common.use_utf8)
    {
        std::string utf8_submitted_text = Common::UTF16ToUTF8(submitted_text);

        LOG_DEBUG(Service_AM, "\nSwkbdResult: {}\nUTF-8 Submitted Text: {}", result, utf8_submitted_text);

        std::memcpy(out_data.data(), &result, sizeof(SwkbdResult));
        std::memcpy(out_data.data() + sizeof(SwkbdResult), utf8_submitted_text.data(), utf8_submitted_text.size());
    }
    else
    {
        LOG_DEBUG(Service_AM, "\nSwkbdResult: {}\nUTF-16 Submitted Text: {}", result,
                  Common::UTF16ToUTF8(submitted_text));

        std::memcpy(out_data.data(), &result, sizeof(SwkbdResult));
        std::memcpy(out_data.data() + sizeof(SwkbdResult), submitted_text.data(),
                    submitted_text.size() * sizeof(char16_t));
    }

    PushOutData(std::make_shared<IStorage>(system, std::move(out_data)));

    ExitKeyboard();
}

void SoftwareKeyboard::SubmitForTextCheck(std::u16string submitted_text)
{
    current_text = std::move(submitted_text);

    std::vector<u8> out_data(sizeof(u64) + STRING_BUFFER_SIZE);

    if (swkbd_config_common.use_utf8)
    {
        std::string utf8_submitted_text = Common::UTF16ToUTF8(current_text);
        // Include the null terminator in the buffer size.
        const u64 buffer_size = utf8_submitted_text.size() + 1;

        LOG_DEBUG(Service_AM, "\nBuffer Size: {}\nUTF-8 Submitted Text: {}", buffer_size, utf8_submitted_text);

        std::memcpy(out_data.data(), &buffer_size, sizeof(u64));
        std::memcpy(out_data.data() + sizeof(u64), utf8_submitted_text.data(), utf8_submitted_text.size());
    }
    else
    {
        // Include the null terminator in the buffer size.
        const u64 buffer_size = (current_text.size() + 1) * sizeof(char16_t);

        LOG_DEBUG(Service_AM, "\nBuffer Size: {}\nUTF-16 Submitted Text: {}", buffer_size,
                  Common::UTF16ToUTF8(current_text));

        std::memcpy(out_data.data(), &buffer_size, sizeof(u64));
        std::memcpy(out_data.data() + sizeof(u64), current_text.data(),
                    current_text.size() * sizeof(char16_t));
    }

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(out_data)));
}

void SoftwareKeyboard::SendReply(SwkbdReplyType reply_type)
{
    switch (reply_type)
    {
    case SwkbdReplyType::FinishedInitialize:
        ReplyFinishedInitialize();
        break;
    case SwkbdReplyType::Default:
        ReplyDefault();
        break;
    case SwkbdReplyType::ChangedString:
        ReplyChangedString();
        break;
    case SwkbdReplyType::MovedCursor:
        ReplyMovedCursor();
        break;
    case SwkbdReplyType::MovedTab:
        ReplyMovedTab();
        break;
    case SwkbdReplyType::DecidedEnter:
        ReplyDecidedEnter();
        break;
    case SwkbdReplyType::DecidedCancel:
        ReplyDecidedCancel();
        break;
    case SwkbdReplyType::ChangedStringUtf8:
        ReplyChangedStringUtf8();
        break;
    case SwkbdReplyType::MovedCursorUtf8:
        ReplyMovedCursorUtf8();
        break;
    case SwkbdReplyType::DecidedEnterUtf8:
        ReplyDecidedEnterUtf8();
        break;
    case SwkbdReplyType::UnsetCustomizeDic:
        ReplyUnsetCustomizeDic();
        break;
    case SwkbdReplyType::ReleasedUserWordInfo:
        ReplyReleasedUserWordInfo();
        break;
    case SwkbdReplyType::UnsetCustomizedDictionaries:
        ReplyUnsetCustomizedDictionaries();
        break;
    case SwkbdReplyType::ChangedStringV2:
        ReplyChangedStringV2();
        break;
    case SwkbdReplyType::MovedCursorV2:
        ReplyMovedCursorV2();
        break;
    case SwkbdReplyType::ChangedStringUtf8V2:
        ReplyChangedStringUtf8V2();
        break;
    case SwkbdReplyType::MovedCursorUtf8V2:
        ReplyMovedCursorUtf8V2();
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown SwkbdReplyType={}", reply_type);
        ReplyDefault();
        break;
    }
}

void SoftwareKeyboard::ChangeState(SwkbdState state)
{
    swkbd_state = state;

    ReplyDefault();
}

void SoftwareKeyboard::InitializeFrontendNormalKeyboard()
{
    std::u16string ok_text = Common::UTF16StringFromFixedZeroTerminatedBuffer(swkbd_config_common.ok_text.data(), swkbd_config_common.ok_text.size());
    std::u16string header_text = Common::UTF16StringFromFixedZeroTerminatedBuffer(swkbd_config_common.header_text.data(), swkbd_config_common.header_text.size());
    std::u16string sub_text = Common::UTF16StringFromFixedZeroTerminatedBuffer(swkbd_config_common.sub_text.data(), swkbd_config_common.sub_text.size());
    std::u16string guide_text = Common::UTF16StringFromFixedZeroTerminatedBuffer(swkbd_config_common.guide_text.data(), swkbd_config_common.guide_text.size());

    const u32 max_text_length = swkbd_config_common.max_text_length > 0 && swkbd_config_common.max_text_length <= DEFAULT_MAX_TEXT_LENGTH ? swkbd_config_common.max_text_length : DEFAULT_MAX_TEXT_LENGTH;
    const u32 min_text_length = swkbd_config_common.min_text_length <= max_text_length ? swkbd_config_common.min_text_length : 0;

    const s32 initial_cursor_position = [this] {
        switch (swkbd_config_common.initial_cursor_position)
        {
        case SwkbdInitialCursorPosition::Start:
        default:
            return 0;
        case SwkbdInitialCursorPosition::End:
            return static_cast<s32>(initial_text.size());
        }
    }();

    const auto text_draw_type = [this, max_text_length] {
        switch (swkbd_config_common.text_draw_type)
        {
        case SwkbdTextDrawType::Line:
        default:
            return max_text_length <= 32 ? SwkbdTextDrawType::Line : SwkbdTextDrawType::Box;
        case SwkbdTextDrawType::Box:
        case SwkbdTextDrawType::DownloadCode:
            return swkbd_config_common.text_draw_type;
        }
    }();

    const auto enable_return_button = text_draw_type == SwkbdTextDrawType::Box ? swkbd_config_common.enable_return_button : false;

    const auto disable_cancel_button = swkbd_applet_version >= SwkbdAppletVersion::Version393227 ? swkbd_config_new.disable_cancel_button : false;

    KeyboardInitializeParameters host_parameters{};
    CopyUtf16ToHostBuffer(host_parameters.ok_text, std::size(host_parameters.ok_text), &host_parameters.ok_text_unit_count, ok_text);
    CopyUtf16ToHostBuffer(host_parameters.header_text, std::size(host_parameters.header_text), &host_parameters.header_text_unit_count, header_text);
    CopyUtf16ToHostBuffer(host_parameters.sub_text, std::size(host_parameters.sub_text), &host_parameters.sub_text_unit_count, sub_text);
    CopyUtf16ToHostBuffer(host_parameters.guide_text, std::size(host_parameters.guide_text), &host_parameters.guide_text_unit_count, guide_text);
    CopyUtf16ToHostBuffer(host_parameters.initial_text, std::size(host_parameters.initial_text), &host_parameters.initial_text_unit_count, initial_text);
    host_parameters.left_optional_symbol_key = static_cast<uint16_t>(swkbd_config_common.left_optional_symbol_key);
    host_parameters.right_optional_symbol_key = static_cast<uint16_t>(swkbd_config_common.right_optional_symbol_key);
    host_parameters.max_text_length = max_text_length;
    host_parameters.min_text_length = min_text_length;
    host_parameters.initial_cursor_position = initial_cursor_position;
    host_parameters.type = static_cast<SwkbdTypeHost>(static_cast<uint32_t>(swkbd_config_common.type));
    host_parameters.password_mode = static_cast<SwkbdPasswordModeHost>(static_cast<uint32_t>(swkbd_config_common.password_mode));
    host_parameters.text_draw_type = static_cast<SwkbdTextDrawTypeHost>(static_cast<uint32_t>(text_draw_type));
    host_parameters.key_disable_flags_raw = swkbd_config_common.key_disable_flags.raw;
    host_parameters.use_blur_background = swkbd_config_common.use_blur_background;
    host_parameters.enable_backspace_button = true;
    host_parameters.enable_return_button = enable_return_button;
    host_parameters.disable_cancel_button = disable_cancel_button;

    frontend.InitializeKeyboard(false, &host_parameters, this, SoftwareKeyboardSubmitNormalThunk, nullptr, nullptr);
}

void SoftwareKeyboard::InitializeFrontendInlineKeyboard(const KeyboardInitializeParameters * initialize_parameters)
{
    frontend.InitializeKeyboard(true, initialize_parameters, nullptr, nullptr, this, SoftwareKeyboardSubmitInlineThunk);
}

void SoftwareKeyboard::InitializeFrontendInlineKeyboardOld()
{
    const auto & appear_arg = swkbd_calc_arg_old.appear_arg;

    std::u16string ok_text = Common::UTF16StringFromFixedZeroTerminatedBuffer(
        appear_arg.ok_text.data(), appear_arg.ok_text.size());

    const u32 max_text_length =
        appear_arg.max_text_length > 0 && appear_arg.max_text_length <= DEFAULT_MAX_TEXT_LENGTH
            ? appear_arg.max_text_length
            : DEFAULT_MAX_TEXT_LENGTH;

    const u32 min_text_length =
        appear_arg.min_text_length <= max_text_length ? appear_arg.min_text_length : 0;

    const s32 initial_cursor_position = current_cursor_position > 0 ? current_cursor_position : 0;

    const auto text_draw_type =
        max_text_length <= 32 ? SwkbdTextDrawType::Line : SwkbdTextDrawType::Box;

    KeyboardInitializeParameters host_parameters{};
    CopyUtf16ToHostBuffer(host_parameters.ok_text, std::size(host_parameters.ok_text), &host_parameters.ok_text_unit_count, ok_text);
    CopyUtf16ToHostBuffer(host_parameters.initial_text, std::size(host_parameters.initial_text), &host_parameters.initial_text_unit_count, current_text);
    host_parameters.left_optional_symbol_key = static_cast<uint16_t>(appear_arg.left_optional_symbol_key);
    host_parameters.right_optional_symbol_key = static_cast<uint16_t>(appear_arg.right_optional_symbol_key);
    host_parameters.max_text_length = max_text_length;
    host_parameters.min_text_length = min_text_length;
    host_parameters.initial_cursor_position = initial_cursor_position;
    host_parameters.type = static_cast<SwkbdTypeHost>(static_cast<uint32_t>(appear_arg.type));
    host_parameters.password_mode = SwkbdPasswordModeHost::Disabled;
    host_parameters.text_draw_type = static_cast<SwkbdTextDrawTypeHost>(static_cast<uint32_t>(text_draw_type));
    host_parameters.key_disable_flags_raw = appear_arg.key_disable_flags.raw;
    host_parameters.use_blur_background = false;
    host_parameters.enable_backspace_button = swkbd_calc_arg_old.enable_backspace_button;
    host_parameters.enable_return_button = appear_arg.enable_return_button;
    host_parameters.disable_cancel_button = appear_arg.disable_cancel_button;

    InitializeFrontendInlineKeyboard(&host_parameters);
}

void SoftwareKeyboard::InitializeFrontendInlineKeyboardNew()
{
    const auto & appear_arg = swkbd_calc_arg_new.appear_arg;

    std::u16string ok_text = Common::UTF16StringFromFixedZeroTerminatedBuffer(
        appear_arg.ok_text.data(), appear_arg.ok_text.size());

    const u32 max_text_length =
        appear_arg.max_text_length > 0 && appear_arg.max_text_length <= DEFAULT_MAX_TEXT_LENGTH
            ? appear_arg.max_text_length
            : DEFAULT_MAX_TEXT_LENGTH;

    const u32 min_text_length =
        appear_arg.min_text_length <= max_text_length ? appear_arg.min_text_length : 0;

    const s32 initial_cursor_position = current_cursor_position > 0 ? current_cursor_position : 0;

    const auto text_draw_type =
        max_text_length <= 32 ? SwkbdTextDrawType::Line : SwkbdTextDrawType::Box;

    KeyboardInitializeParameters host_parameters{};
    CopyUtf16ToHostBuffer(host_parameters.ok_text, std::size(host_parameters.ok_text), &host_parameters.ok_text_unit_count, ok_text);
    CopyUtf16ToHostBuffer(host_parameters.initial_text, std::size(host_parameters.initial_text), &host_parameters.initial_text_unit_count, current_text);
    host_parameters.left_optional_symbol_key = static_cast<uint16_t>(appear_arg.left_optional_symbol_key);
    host_parameters.right_optional_symbol_key = static_cast<uint16_t>(appear_arg.right_optional_symbol_key);
    host_parameters.max_text_length = max_text_length;
    host_parameters.min_text_length = min_text_length;
    host_parameters.initial_cursor_position = initial_cursor_position;
    host_parameters.type = static_cast<SwkbdTypeHost>(static_cast<uint32_t>(appear_arg.type));
    host_parameters.password_mode = SwkbdPasswordModeHost::Disabled;
    host_parameters.text_draw_type = static_cast<SwkbdTextDrawTypeHost>(static_cast<uint32_t>(text_draw_type));
    host_parameters.key_disable_flags_raw = appear_arg.key_disable_flags.raw;
    host_parameters.use_blur_background = false;
    host_parameters.enable_backspace_button = swkbd_calc_arg_new.enable_backspace_button;
    host_parameters.enable_return_button = appear_arg.enable_return_button;
    host_parameters.disable_cancel_button = appear_arg.disable_cancel_button;

    InitializeFrontendInlineKeyboard(&host_parameters);
}

void SoftwareKeyboard::ShowNormalKeyboard()
{
    frontend.ShowNormalKeyboard();
}

void SoftwareKeyboard::ShowTextCheckDialog(SwkbdTextCheckResult text_check_result,
                                           std::u16string text_check_message)
{
    frontend.ShowTextCheckDialog(static_cast<uint32_t>(text_check_result), reinterpret_cast<const uint16_t *>(text_check_message.data()), static_cast<uint32_t>(text_check_message.size()));
}

void SoftwareKeyboard::ShowInlineKeyboard(const InlineAppearParameters * appear_parameters)
{
    frontend.ShowInlineKeyboard(appear_parameters);

    ChangeState(SwkbdState::InitializedIsShown);
}

void SoftwareKeyboard::ShowInlineKeyboardOld()
{
    if (swkbd_state != SwkbdState::InitializedIsHidden)
    {
        return;
    }

    ChangeState(SwkbdState::InitializedIsAppearing);

    const auto & appear_arg = swkbd_calc_arg_old.appear_arg;

    const u32 max_text_length =
        appear_arg.max_text_length > 0 && appear_arg.max_text_length <= DEFAULT_MAX_TEXT_LENGTH
            ? appear_arg.max_text_length
            : DEFAULT_MAX_TEXT_LENGTH;

    const u32 min_text_length =
        appear_arg.min_text_length <= max_text_length ? appear_arg.min_text_length : 0;

    InlineAppearParameters appear_parameters{};
    appear_parameters.max_text_length = max_text_length;
    appear_parameters.min_text_length = min_text_length;
    appear_parameters.key_top_scale_x = swkbd_calc_arg_old.key_top_scale_x;
    appear_parameters.key_top_scale_y = swkbd_calc_arg_old.key_top_scale_y;
    appear_parameters.key_top_translate_x = swkbd_calc_arg_old.key_top_translate_x;
    appear_parameters.key_top_translate_y = swkbd_calc_arg_old.key_top_translate_y;
    appear_parameters.type = static_cast<SwkbdTypeHost>(static_cast<uint32_t>(appear_arg.type));
    appear_parameters.key_disable_flags_raw = appear_arg.key_disable_flags.raw;
    appear_parameters.key_top_as_floating = swkbd_calc_arg_old.key_top_as_floating;
    appear_parameters.enable_backspace_button = swkbd_calc_arg_old.enable_backspace_button;
    appear_parameters.enable_return_button = appear_arg.enable_return_button;
    appear_parameters.disable_cancel_button = appear_arg.disable_cancel_button;

    ShowInlineKeyboard(&appear_parameters);
}

void SoftwareKeyboard::ShowInlineKeyboardNew()
{
    if (swkbd_state != SwkbdState::InitializedIsHidden)
    {
        return;
    }

    ChangeState(SwkbdState::InitializedIsAppearing);

    const auto & appear_arg = swkbd_calc_arg_new.appear_arg;

    const u32 max_text_length =
        appear_arg.max_text_length > 0 && appear_arg.max_text_length <= DEFAULT_MAX_TEXT_LENGTH
            ? appear_arg.max_text_length
            : DEFAULT_MAX_TEXT_LENGTH;

    const u32 min_text_length =
        appear_arg.min_text_length <= max_text_length ? appear_arg.min_text_length : 0;

    InlineAppearParameters appear_parameters{};
    appear_parameters.max_text_length = max_text_length;
    appear_parameters.min_text_length = min_text_length;
    appear_parameters.key_top_scale_x = swkbd_calc_arg_new.key_top_scale_x;
    appear_parameters.key_top_scale_y = swkbd_calc_arg_new.key_top_scale_y;
    appear_parameters.key_top_translate_x = swkbd_calc_arg_new.key_top_translate_x;
    appear_parameters.key_top_translate_y = swkbd_calc_arg_new.key_top_translate_y;
    appear_parameters.type = static_cast<SwkbdTypeHost>(static_cast<uint32_t>(appear_arg.type));
    appear_parameters.key_disable_flags_raw = appear_arg.key_disable_flags.raw;
    appear_parameters.key_top_as_floating = swkbd_calc_arg_new.key_top_as_floating;
    appear_parameters.enable_backspace_button = swkbd_calc_arg_new.enable_backspace_button;
    appear_parameters.enable_return_button = appear_arg.enable_return_button;
    appear_parameters.disable_cancel_button = appear_arg.disable_cancel_button;

    ShowInlineKeyboard(&appear_parameters);
}

void SoftwareKeyboard::HideInlineKeyboard()
{
    if (swkbd_state != SwkbdState::InitializedIsShown)
    {
        return;
    }

    ChangeState(SwkbdState::InitializedIsDisappearing);

    frontend.HideInlineKeyboard();

    ChangeState(SwkbdState::InitializedIsHidden);
}

void SoftwareKeyboard::InlineTextChanged()
{
    InlineTextHostParameters host_parameters{};
    const std::size_t n = std::min(current_text.size(), static_cast<std::size_t>(0x3EA));
    for (std::size_t i = 0; i < n; ++i)
    {
        host_parameters.input_text[i] = static_cast<uint16_t>(current_text[i]);
    }
    host_parameters.input_text_unit_count = static_cast<uint32_t>(n);
    host_parameters.cursor_position = current_cursor_position;

    frontend.InlineTextChanged(&host_parameters);
}

void SoftwareKeyboard::ExitKeyboard()
{
    complete = true;
    status = ResultSuccess;

    frontend.ExitKeyboard();

    Exit();
}

Result SoftwareKeyboard::RequestExit()
{
    frontend.Close();
    R_SUCCEED();
}

// Inline Software Keyboard Requests

void SoftwareKeyboard::RequestFinalize(const std::vector<u8> & request_data)
{
    LOG_DEBUG(Service_AM, "Processing Request: Finalize");

    ChangeState(SwkbdState::NotInitialized);

    ExitKeyboard();
}

void SoftwareKeyboard::RequestSetUserWordInfo(const std::vector<u8> & request_data)
{
    LOG_WARNING(Service_AM, "SetUserWordInfo is not implemented.");

    ReplyReleasedUserWordInfo();
}

void SoftwareKeyboard::RequestSetCustomizeDic(const std::vector<u8> & request_data)
{
    LOG_WARNING(Service_AM, "SetCustomizeDic is not implemented.");
}

void SoftwareKeyboard::RequestCalc(const std::vector<u8> & request_data)
{
    LOG_DEBUG(Service_AM, "Processing Request: Calc");

    ASSERT(request_data.size() >= sizeof(SwkbdRequestCommand) + sizeof(SwkbdCalcArgCommon));

    std::memcpy(&swkbd_calc_arg_common, request_data.data() + sizeof(SwkbdRequestCommand),
                sizeof(SwkbdCalcArgCommon));

    switch (swkbd_calc_arg_common.calc_arg_size)
    {
    case sizeof(SwkbdCalcArgCommon) + sizeof(SwkbdCalcArgOld):
        ASSERT(request_data.size() ==
               sizeof(SwkbdRequestCommand) + sizeof(SwkbdCalcArgCommon) + sizeof(SwkbdCalcArgOld));
        std::memcpy(&swkbd_calc_arg_old,
                    request_data.data() + sizeof(SwkbdRequestCommand) + sizeof(SwkbdCalcArgCommon),
                    sizeof(SwkbdCalcArgOld));
        RequestCalcOld();
        break;
    case sizeof(SwkbdCalcArgCommon) + sizeof(SwkbdCalcArgNew):
        ASSERT(request_data.size() ==
               sizeof(SwkbdRequestCommand) + sizeof(SwkbdCalcArgCommon) + sizeof(SwkbdCalcArgNew));
        std::memcpy(&swkbd_calc_arg_new,
                    request_data.data() + sizeof(SwkbdRequestCommand) + sizeof(SwkbdCalcArgCommon),
                    sizeof(SwkbdCalcArgNew));
        RequestCalcNew();
        break;
    default:
        UNIMPLEMENTED_MSG("Unknown SwkbdCalcArg size={}", swkbd_calc_arg_common.calc_arg_size);
        ASSERT(request_data.size() >=
               sizeof(SwkbdRequestCommand) + sizeof(SwkbdCalcArgCommon) + sizeof(SwkbdCalcArgNew));
        std::memcpy(&swkbd_calc_arg_new,
                    request_data.data() + sizeof(SwkbdRequestCommand) + sizeof(SwkbdCalcArgCommon),
                    sizeof(SwkbdCalcArgNew));
        RequestCalcNew();
        break;
    }
}

void SoftwareKeyboard::RequestCalcOld()
{
    if (swkbd_calc_arg_common.flags.set_input_text)
    {
        current_text = Common::UTF16StringFromFixedZeroTerminatedBuffer(
            swkbd_calc_arg_old.input_text.data(), swkbd_calc_arg_old.input_text.size());
    }

    if (swkbd_calc_arg_common.flags.set_cursor_position)
    {
        current_cursor_position = swkbd_calc_arg_old.cursor_position;
    }

    if (swkbd_calc_arg_common.flags.set_utf8_mode)
    {
        inline_use_utf8 = swkbd_calc_arg_old.utf8_mode;
    }

    if (swkbd_state <= SwkbdState::InitializedIsHidden &&
        swkbd_calc_arg_common.flags.unset_customize_dic)
    {
        ReplyUnsetCustomizeDic();
    }

    if (swkbd_state <= SwkbdState::InitializedIsHidden &&
        swkbd_calc_arg_common.flags.unset_user_word_info)
    {
        ReplyReleasedUserWordInfo();
    }

    if (swkbd_state == SwkbdState::NotInitialized &&
        swkbd_calc_arg_common.flags.set_initialize_arg)
    {
        InitializeFrontendInlineKeyboardOld();

        ChangeState(SwkbdState::InitializedIsHidden);

        ReplyFinishedInitialize();
    }

    if (!swkbd_calc_arg_common.flags.set_initialize_arg &&
        (swkbd_calc_arg_common.flags.set_input_text ||
         swkbd_calc_arg_common.flags.set_cursor_position))
    {
        InlineTextChanged();
    }

    if (swkbd_state == SwkbdState::InitializedIsHidden && swkbd_calc_arg_common.flags.appear)
    {
        ShowInlineKeyboardOld();
        return;
    }

    if (swkbd_state == SwkbdState::InitializedIsShown && swkbd_calc_arg_common.flags.disappear)
    {
        HideInlineKeyboard();
        return;
    }
}

void SoftwareKeyboard::RequestCalcNew()
{
    if (swkbd_calc_arg_common.flags.set_input_text)
    {
        current_text = Common::UTF16StringFromFixedZeroTerminatedBuffer(
            swkbd_calc_arg_new.input_text.data(), swkbd_calc_arg_new.input_text.size());
    }

    if (swkbd_calc_arg_common.flags.set_cursor_position)
    {
        current_cursor_position = swkbd_calc_arg_new.cursor_position;
    }

    if (swkbd_calc_arg_common.flags.set_utf8_mode)
    {
        inline_use_utf8 = swkbd_calc_arg_new.utf8_mode;
    }

    if (swkbd_state <= SwkbdState::InitializedIsHidden &&
        swkbd_calc_arg_common.flags.unset_customize_dic)
    {
        ReplyUnsetCustomizeDic();
    }

    if (swkbd_state <= SwkbdState::InitializedIsHidden &&
        swkbd_calc_arg_common.flags.unset_user_word_info)
    {
        ReplyReleasedUserWordInfo();
    }

    if (swkbd_state == SwkbdState::NotInitialized &&
        swkbd_calc_arg_common.flags.set_initialize_arg)
    {
        InitializeFrontendInlineKeyboardNew();

        ChangeState(SwkbdState::InitializedIsHidden);

        ReplyFinishedInitialize();
    }

    if (!swkbd_calc_arg_common.flags.set_initialize_arg &&
        (swkbd_calc_arg_common.flags.set_input_text ||
         swkbd_calc_arg_common.flags.set_cursor_position))
    {
        InlineTextChanged();
    }

    if (swkbd_state == SwkbdState::InitializedIsHidden && swkbd_calc_arg_common.flags.appear)
    {
        ShowInlineKeyboardNew();
        return;
    }

    if (swkbd_state == SwkbdState::InitializedIsShown && swkbd_calc_arg_common.flags.disappear)
    {
        HideInlineKeyboard();
        return;
    }
}

void SoftwareKeyboard::RequestSetCustomizedDictionaries(const std::vector<u8> & request_data)
{
    LOG_WARNING(Service_AM, "SetCustomizedDictionaries is not implemented.");
}

void SoftwareKeyboard::RequestUnsetCustomizedDictionaries(const std::vector<u8> & request_data)
{
    LOG_WARNING(Service_AM, "(STUBBED) Processing Request: UnsetCustomizedDictionaries");

    ReplyUnsetCustomizedDictionaries();
}

void SoftwareKeyboard::RequestSetChangedStringV2Flag(const std::vector<u8> & request_data)
{
    LOG_DEBUG(Service_AM, "Processing Request: SetChangedStringV2Flag");

    ASSERT(request_data.size() == sizeof(SwkbdRequestCommand) + 1);

    std::memcpy(&use_changed_string_v2, request_data.data() + sizeof(SwkbdRequestCommand), 1);
}

void SoftwareKeyboard::RequestSetMovedCursorV2Flag(const std::vector<u8> & request_data)
{
    LOG_DEBUG(Service_AM, "Processing Request: SetMovedCursorV2Flag");

    ASSERT(request_data.size() == sizeof(SwkbdRequestCommand) + 1);

    std::memcpy(&use_moved_cursor_v2, request_data.data() + sizeof(SwkbdRequestCommand), 1);
}

// Inline Software Keyboard Replies

void SoftwareKeyboard::ReplyFinishedInitialize()
{
    LOG_DEBUG(Service_AM, "Sending Reply: FinishedInitialize");

    std::vector<u8> reply(REPLY_BASE_SIZE + 1);

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::FinishedInitialize);

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyDefault()
{
    LOG_DEBUG(Service_AM, "Sending Reply: Default");

    std::vector<u8> reply(REPLY_BASE_SIZE);

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::Default);

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyChangedString()
{
    LOG_DEBUG(Service_AM, "Sending Reply: ChangedString");

    std::vector<u8> reply(REPLY_BASE_SIZE + REPLY_UTF16_SIZE + sizeof(SwkbdChangedStringArg));

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::ChangedString);

    const SwkbdChangedStringArg changed_string_arg{
        .text_length{static_cast<u32>(current_text.size())},
        .dictionary_start_cursor_position{-1},
        .dictionary_end_cursor_position{-1},
        .cursor_position{current_cursor_position},
    };

    std::memcpy(reply.data() + REPLY_BASE_SIZE, current_text.data(),
                current_text.size() * sizeof(char16_t));
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF16_SIZE, &changed_string_arg,
                sizeof(SwkbdChangedStringArg));

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyMovedCursor()
{
    LOG_DEBUG(Service_AM, "Sending Reply: MovedCursor");

    std::vector<u8> reply(REPLY_BASE_SIZE + REPLY_UTF16_SIZE + sizeof(SwkbdMovedCursorArg));

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::MovedCursor);

    const SwkbdMovedCursorArg moved_cursor_arg{
        .text_length{static_cast<u32>(current_text.size())},
        .cursor_position{current_cursor_position},
    };

    std::memcpy(reply.data() + REPLY_BASE_SIZE, current_text.data(),
                current_text.size() * sizeof(char16_t));
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF16_SIZE, &moved_cursor_arg,
                sizeof(SwkbdMovedCursorArg));

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyMovedTab()
{
    LOG_DEBUG(Service_AM, "Sending Reply: MovedTab");

    std::vector<u8> reply(REPLY_BASE_SIZE + REPLY_UTF16_SIZE + sizeof(SwkbdMovedTabArg));

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::MovedTab);

    const SwkbdMovedTabArg moved_tab_arg{
        .text_length{static_cast<u32>(current_text.size())},
        .cursor_position{current_cursor_position},
    };

    std::memcpy(reply.data() + REPLY_BASE_SIZE, current_text.data(), current_text.size() * sizeof(char16_t));
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF16_SIZE, &moved_tab_arg, sizeof(SwkbdMovedTabArg));

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyDecidedEnter()
{
    LOG_DEBUG(Service_AM, "Sending Reply: DecidedEnter");

    std::vector<u8> reply(REPLY_BASE_SIZE + REPLY_UTF16_SIZE + sizeof(SwkbdDecidedEnterArg));

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::DecidedEnter);

    const SwkbdDecidedEnterArg decided_enter_arg{
        .text_length{static_cast<u32>(current_text.size())},
    };

    std::memcpy(reply.data() + REPLY_BASE_SIZE, current_text.data(), current_text.size() * sizeof(char16_t));
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF16_SIZE, &decided_enter_arg, sizeof(SwkbdDecidedEnterArg));

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));

    HideInlineKeyboard();
}

void SoftwareKeyboard::ReplyDecidedCancel()
{
    LOG_DEBUG(Service_AM, "Sending Reply: DecidedCancel");

    std::vector<u8> reply(REPLY_BASE_SIZE);

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::DecidedCancel);

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));

    HideInlineKeyboard();
}

void SoftwareKeyboard::ReplyChangedStringUtf8()
{
    LOG_DEBUG(Service_AM, "Sending Reply: ChangedStringUtf8");

    std::vector<u8> reply(REPLY_BASE_SIZE + REPLY_UTF8_SIZE + sizeof(SwkbdChangedStringArg));

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::ChangedStringUtf8);

    std::string utf8_current_text = Common::UTF16ToUTF8(current_text);

    const SwkbdChangedStringArg changed_string_arg{
        .text_length{static_cast<u32>(current_text.size())},
        .dictionary_start_cursor_position{-1},
        .dictionary_end_cursor_position{-1},
        .cursor_position{current_cursor_position},
    };

    std::memcpy(reply.data() + REPLY_BASE_SIZE, utf8_current_text.data(), utf8_current_text.size());
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF8_SIZE, &changed_string_arg, sizeof(SwkbdChangedStringArg));

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyMovedCursorUtf8()
{
    LOG_DEBUG(Service_AM, "Sending Reply: MovedCursorUtf8");

    std::vector<u8> reply(REPLY_BASE_SIZE + REPLY_UTF8_SIZE + sizeof(SwkbdMovedCursorArg));

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::MovedCursorUtf8);

    std::string utf8_current_text = Common::UTF16ToUTF8(current_text);

    const SwkbdMovedCursorArg moved_cursor_arg{
        .text_length{static_cast<u32>(current_text.size())},
        .cursor_position{current_cursor_position},
    };

    std::memcpy(reply.data() + REPLY_BASE_SIZE, utf8_current_text.data(), utf8_current_text.size());
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF8_SIZE, &moved_cursor_arg, sizeof(SwkbdMovedCursorArg));

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyDecidedEnterUtf8()
{
    LOG_DEBUG(Service_AM, "Sending Reply: DecidedEnterUtf8");

    std::vector<u8> reply(REPLY_BASE_SIZE + REPLY_UTF8_SIZE + sizeof(SwkbdDecidedEnterArg));

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::DecidedEnterUtf8);

    std::string utf8_current_text = Common::UTF16ToUTF8(current_text);

    const SwkbdDecidedEnterArg decided_enter_arg{
        .text_length{static_cast<u32>(current_text.size())},
    };

    std::memcpy(reply.data() + REPLY_BASE_SIZE, utf8_current_text.data(), utf8_current_text.size());
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF8_SIZE, &decided_enter_arg, sizeof(SwkbdDecidedEnterArg));

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));

    HideInlineKeyboard();
}

void SoftwareKeyboard::ReplyUnsetCustomizeDic()
{
    LOG_DEBUG(Service_AM, "Sending Reply: UnsetCustomizeDic");

    std::vector<u8> reply(REPLY_BASE_SIZE);

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::UnsetCustomizeDic);

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyReleasedUserWordInfo()
{
    LOG_DEBUG(Service_AM, "Sending Reply: ReleasedUserWordInfo");

    std::vector<u8> reply(REPLY_BASE_SIZE);

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::ReleasedUserWordInfo);

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyUnsetCustomizedDictionaries()
{
    LOG_DEBUG(Service_AM, "Sending Reply: UnsetCustomizedDictionaries");

    std::vector<u8> reply(REPLY_BASE_SIZE);

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::UnsetCustomizedDictionaries);

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyChangedStringV2()
{
    LOG_DEBUG(Service_AM, "Sending Reply: ChangedStringV2");

    std::vector<u8> reply(REPLY_BASE_SIZE + REPLY_UTF16_SIZE + sizeof(SwkbdChangedStringArg) + 1);

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::ChangedStringV2);

    const SwkbdChangedStringArg changed_string_arg{
        .text_length{static_cast<u32>(current_text.size())},
        .dictionary_start_cursor_position{-1},
        .dictionary_end_cursor_position{-1},
        .cursor_position{current_cursor_position},
    };

    constexpr u8 flag = 0;

    std::memcpy(reply.data() + REPLY_BASE_SIZE, current_text.data(), current_text.size() * sizeof(char16_t));
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF16_SIZE, &changed_string_arg, sizeof(SwkbdChangedStringArg));
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF16_SIZE + sizeof(SwkbdChangedStringArg), &flag, 1);

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyMovedCursorV2()
{
    LOG_DEBUG(Service_AM, "Sending Reply: MovedCursorV2");

    std::vector<u8> reply(REPLY_BASE_SIZE + REPLY_UTF16_SIZE + sizeof(SwkbdMovedCursorArg) + 1);

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::MovedCursorV2);

    const SwkbdMovedCursorArg moved_cursor_arg{
        .text_length{static_cast<u32>(current_text.size())},
        .cursor_position{current_cursor_position},
    };

    constexpr u8 flag = 0;

    std::memcpy(reply.data() + REPLY_BASE_SIZE, current_text.data(), current_text.size() * sizeof(char16_t));
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF16_SIZE, &moved_cursor_arg, sizeof(SwkbdMovedCursorArg));
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF16_SIZE + sizeof(SwkbdMovedCursorArg), &flag, 1);

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyChangedStringUtf8V2()
{
    LOG_DEBUG(Service_AM, "Sending Reply: ChangedStringUtf8V2");

    std::vector<u8> reply(REPLY_BASE_SIZE + REPLY_UTF8_SIZE + sizeof(SwkbdChangedStringArg) + 1);

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::ChangedStringUtf8V2);

    std::string utf8_current_text = Common::UTF16ToUTF8(current_text);

    const SwkbdChangedStringArg changed_string_arg{
        .text_length{static_cast<u32>(current_text.size())},
        .dictionary_start_cursor_position{-1},
        .dictionary_end_cursor_position{-1},
        .cursor_position{current_cursor_position},
    };

    constexpr u8 flag = 0;

    std::memcpy(reply.data() + REPLY_BASE_SIZE, utf8_current_text.data(), utf8_current_text.size());
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF8_SIZE, &changed_string_arg, sizeof(SwkbdChangedStringArg));
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF8_SIZE + sizeof(SwkbdChangedStringArg), &flag, 1);

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

void SoftwareKeyboard::ReplyMovedCursorUtf8V2()
{
    LOG_DEBUG(Service_AM, "Sending Reply: MovedCursorUtf8V2");

    std::vector<u8> reply(REPLY_BASE_SIZE + REPLY_UTF8_SIZE + sizeof(SwkbdMovedCursorArg) + 1);

    SetReplyBase(reply, swkbd_state, SwkbdReplyType::MovedCursorUtf8V2);

    std::string utf8_current_text = Common::UTF16ToUTF8(current_text);

    const SwkbdMovedCursorArg moved_cursor_arg{
        .text_length{static_cast<u32>(current_text.size())},
        .cursor_position{current_cursor_position},
    };

    constexpr u8 flag = 0;

    std::memcpy(reply.data() + REPLY_BASE_SIZE, utf8_current_text.data(), utf8_current_text.size());
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF8_SIZE, &moved_cursor_arg, sizeof(SwkbdMovedCursorArg));
    std::memcpy(reply.data() + REPLY_BASE_SIZE + REPLY_UTF8_SIZE + sizeof(SwkbdMovedCursorArg), &flag, 1);

    PushInteractiveOutData(std::make_shared<IStorage>(system, std::move(reply)));
}

} // namespace Service::AM::Frontend
