#include "plural_rule.h"

namespace
{
class Evaluation
{
public:
    Evaluation(const std::string & expression, uint64_t count) :
        m_text(expression),
        m_count(count)
    {
    }

    bool Run(uint64_t & value)
    {
        value = Conditional();
        Skip();
        return !m_failed && m_at == m_text.size();
    }

private:
    void Skip()
    {
        while (m_at < m_text.size() && m_text[m_at] == ' ')
        {
            ++m_at;
        }
    }

    bool Take(const char * token)
    {
        Skip();
        const std::string expected = token;
        if (m_text.compare(m_at, expected.size(), expected) != 0)
        {
            return false;
        }
        // "<" is not the start of "<=", nor "!" of "!=".
        const char next = m_at + expected.size() < m_text.size() ? m_text[m_at + expected.size()] : '\0';
        if ((expected == "<" || expected == ">" || expected == "!") && next == '=')
        {
            return false;
        }
        m_at += expected.size();
        return true;
    }

    uint64_t Conditional()
    {
        const uint64_t condition = Or();
        if (!Take("?"))
        {
            return condition;
        }
        const uint64_t yes = Conditional();
        if (!Take(":"))
        {
            m_failed = true;
        }
        const uint64_t no = Conditional();
        return condition != 0 ? yes : no;
    }

    uint64_t Or()
    {
        uint64_t value = And();
        while (Take("||"))
        {
            const uint64_t right = And();
            value = value != 0 || right != 0;
        }
        return value;
    }

    uint64_t And()
    {
        uint64_t value = Equality();
        while (Take("&&"))
        {
            const uint64_t right = Equality();
            value = value != 0 && right != 0;
        }
        return value;
    }

    uint64_t Equality()
    {
        uint64_t value = Comparison();
        for (;;)
        {
            if (Take("=="))
            {
                value = value == Comparison();
            }
            else if (Take("!="))
            {
                value = value != Comparison();
            }
            else
            {
                return value;
            }
        }
    }

    uint64_t Comparison()
    {
        uint64_t value = Remainder();
        for (;;)
        {
            if (Take("<="))
            {
                value = value <= Remainder();
            }
            else if (Take(">="))
            {
                value = value >= Remainder();
            }
            else if (Take("<"))
            {
                value = value < Remainder();
            }
            else if (Take(">"))
            {
                value = value > Remainder();
            }
            else
            {
                return value;
            }
        }
    }

    uint64_t Remainder()
    {
        uint64_t value = Operand();
        while (Take("%"))
        {
            const uint64_t divisor = Operand();
            if (divisor == 0)
            {
                m_failed = true;
                return 0;
            }
            value %= divisor;
        }
        return value;
    }

    uint64_t Operand()
    {
        if (Take("("))
        {
            const uint64_t value = Conditional();
            if (!Take(")"))
            {
                m_failed = true;
            }
            return value;
        }
        if (Take("!"))
        {
            return Operand() == 0;
        }
        if (Take("n"))
        {
            return m_count;
        }
        Skip();
        const size_t start = m_at;
        uint64_t value = 0;
        while (m_at < m_text.size() && m_text[m_at] >= '0' && m_text[m_at] <= '9')
        {
            value = value * 10 + static_cast<uint64_t>(m_text[m_at++] - '0');
        }
        if (m_at == start)
        {
            m_failed = true;
        }
        return value;
    }

    const std::string & m_text;
    const uint64_t m_count;
    size_t m_at = 0;
    bool m_failed = false;
};
} // namespace

PluralRule::PluralRule(const std::string & pluralForms)
{
    const size_t start = pluralForms.find("plural=");
    if (start == std::string::npos)
    {
        return;
    }
    const size_t from = start + 7;
    const size_t end = pluralForms.find(';', from);
    m_expression = pluralForms.substr(from, end == std::string::npos ? std::string::npos : end - from);
    uint64_t value = 0;
    if (!Evaluation(m_expression, 1).Run(value))
    {
        m_expression.clear();
    }
}

size_t PluralRule::FormOf(uint64_t count) const
{
    uint64_t form = count == 1 ? 0 : 1;
    if (!m_expression.empty())
    {
        Evaluation(m_expression, count).Run(form);
    }
    return static_cast<size_t>(form);
}
