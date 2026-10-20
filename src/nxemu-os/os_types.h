#pragma once
#include <stdint.h>

enum class AppletMode : uint32_t
{
    HLE = 0,
    LLE,
};

enum class AudioMode : uint32_t
{
    Mono = 0,
    Stereo,
    Surround,
};

enum class Language : uint32_t
{
    Japanese = 0,
    EnglishAmerican,
    French,
    German,
    Italian,
    Spanish,
    Chinese,
    Korean,
    Dutch,
    Portuguese,
    Russian,
    Taiwanese,
    EnglishBritish,
    FrenchCanadian,
    SpanishLatin,
    ChineseSimplified,
    ChineseTraditional,
    PortugueseBrazilian,
};

enum class Region : uint32_t
{
    Japan = 0,
    Usa,
    Europe,
    Australia,
    China,
    Korea,
    Taiwan,
};

constexpr int32_t kRegionOfLanguage = -1;

constexpr Region RegionOfLanguage(Language language)
{
    switch (language)
    {
    case Language::Japanese: return Region::Japan;
    case Language::EnglishAmerican:
    case Language::FrenchCanadian:
    case Language::SpanishLatin:
    case Language::PortugueseBrazilian: return Region::Usa;
    case Language::Korean: return Region::Korea;
    case Language::Chinese:
    case Language::ChineseSimplified: return Region::China;
    case Language::Taiwanese:
    case Language::ChineseTraditional: return Region::Taiwan;
    default: return Region::Europe;
    }
}

enum class TimeZone : uint32_t
{
    Auto = 0,
    Default,
    Cet,
    Cst6Cdt,
    Cuba,
    Eet,
    Egypt,
    Eire,
    Est,
    Est5Edt,
    Gb,
    GbEire,
    Gmt,
    GmtPlusZero,
    GmtMinusZero,
    GmtZero,
    Greenwich,
    Hongkong,
    Hst,
    Iceland,
    Iran,
    Israel,
    Jamaica,
    Japan,
    Kwajalein,
    Libya,
    Met,
    Mst,
    Mst7Mdt,
    Navajo,
    Nz,
    NzChat,
    Poland,
    Portugal,
    Prc,
    Pst8Pdt,
    Roc,
    Rok,
    Singapore,
    Turkey,
    Uct,
    Universal,
    Utc,
    WSu,
    Wet,
    Zulu,
};
