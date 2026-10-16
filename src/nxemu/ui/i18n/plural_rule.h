#pragma once
#include <cstdint>
#include <string>

// How a language counts: which of its forms a text takes for a count. A
// gettext catalogue tells it in its heading, as an expression of n in the
// syntax of C: "nplurals=2; plural=(n > 1);" for French.
class PluralRule
{
public:
    // English: one form for one, another for any other count.
    PluralRule() = default;
    // From the "Plural-Forms:" line of a catalogue. A rule that cannot be
    // read counts as English does.
    explicit PluralRule(const std::string & pluralForms);

    // The form for the count, from 0.
    size_t FormOf(uint64_t count) const;

private:
    std::string m_expression; // empty for English
};
