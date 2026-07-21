// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/file_sys/control_metadata.h"
#include "core/file_sys/vfs/vfs.h"
#include <nxemu-module-spec/base.h>
#include "nxemu-os/os_settings_identifiers.h"
#include "yuzu_common/string_util.h"
#include "yuzu_common/swap.h"

extern IModuleSettings * g_settings;

namespace FileSys
{

const std::array<const char *, 16> LANGUAGE_NAMES{{
    "AmericanEnglish",
    "BritishEnglish",
    "Japanese",
    "French",
    "German",
    "LatinAmericanSpanish",
    "Spanish",
    "Italian",
    "Dutch",
    "CanadianFrench",
    "Portuguese",
    "Russian",
    "Korean",
    "TraditionalChinese",
    "SimplifiedChinese",
    "BrazilianPortuguese",
}};

std::string LanguageEntry::GetApplicationName() const
{
    return Common::StringFromFixedZeroTerminatedBuffer(application_name.data(), application_name.size());
}

std::string LanguageEntry::GetDeveloperName() const
{
    return Common::StringFromFixedZeroTerminatedBuffer(developer_name.data(), developer_name.size());
}

constexpr std::array<Language, 18> language_to_codes = {{
    Language::Japanese,
    Language::AmericanEnglish,
    Language::French,
    Language::German,
    Language::Italian,
    Language::Spanish,
    Language::SimplifiedChinese,
    Language::Korean,
    Language::Dutch,
    Language::Portuguese,
    Language::Russian,
    Language::TraditionalChinese,
    Language::BritishEnglish,
    Language::CanadianFrench,
    Language::LatinAmericanSpanish,
    Language::SimplifiedChinese,
    Language::TraditionalChinese,
    Language::BrazilianPortuguese,
}};

NACP::NACP() = default;

NACP::NACP(VirtualFile file)
{
    file->ReadObject(&raw);
    version = Common::StringFromFixedZeroTerminatedBuffer(raw.version_string.data(), raw.version_string.size());
}

NACP::~NACP() = default;

NACP & NACP::operator=(const NACP & other)
{
    if (this != &other)
    {
        raw = other.raw;
        version = other.version;
        developer = other.developer;
    }
    return *this;
}

const LanguageEntry & NACP::GetLanguageEntry() const
{
    Language language = language_to_codes[g_settings->GetInt(NXOsSetting::LanguageIndex)];

    {
        const auto & language_entry = raw.language_entries.at(static_cast<u8>(language));
        if (!language_entry.GetApplicationName().empty())
        {
            return language_entry;
        }
    }

    for (const auto & language_entry : raw.language_entries)
    {
        if (!language_entry.GetApplicationName().empty())
            return language_entry;
    }

    return raw.language_entries.at(static_cast<u8>(Language::AmericanEnglish));
}

std::string NACP::GetApplicationName() const
{
    return GetLanguageEntry().GetApplicationName();
}

const char * NACP::GetDeveloperName() const
{
    if (developer.empty())
    {
        developer = GetLanguageEntry().GetDeveloperName();
    }
    return developer.c_str();
}

uint64_t NACP::GetTitleId() const
{
    return raw.save_data_owner_id;
}

uint64_t NACP::GetDLCBaseTitleId() const
{
    return raw.dlc_base_title_id;
}

const char * NACP::GetVersionString() const
{
    return version.c_str();
}

uint64_t NACP::GetDefaultNormalSaveSize() const
{
    return raw.user_account_save_data_size;
}

uint64_t NACP::GetDefaultJournalSaveSize() const
{
    return raw.user_account_save_data_journal_size;
}

bool NACP::GetUserAccountSwitchLock() const
{
    return raw.user_account_switch_lock != 0;
}

u32 NACP::GetSupportedLanguages() const
{
    return raw.supported_languages;
}

uint64_t NACP::GetDeviceSaveDataSize() const
{
    return raw.device_save_data_size;
}

u32 NACP::GetParentalControlFlag() const
{
    return raw.parental_control;
}

std::vector<u8> NACP::GetRawBytes() const
{
    std::vector<u8> out(sizeof(RawNACP));
    std::memcpy(out.data(), &raw, sizeof(RawNACP));
    return out;
}

bool NACP::GetRatingAge(uint8_t * buffer, uint32_t bufferSize) const
{
    if (bufferSize > raw.rating_age.size())
    {
        return false;
    }
    memcpy(buffer, raw.rating_age.data(), bufferSize);
    return true;
}

void NACP::Release()
{
    delete this;
}

} // namespace FileSys
