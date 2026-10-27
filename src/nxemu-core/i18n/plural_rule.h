#pragma once
#include <cstdint>
#include <string>

class PluralRule
{
public:
    PluralRule() = default;
    explicit PluralRule(const std::string & pluralForms);

    size_t FormOf(uint64_t count) const;

private:
    std::string m_expression; // empty for English
};
