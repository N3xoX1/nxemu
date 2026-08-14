#include "json.h"
#include "std_string.h"
#include <algorithm>
#include <assert.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace
{
template <typename Iter>
Iter fixNumericLocale(Iter begin, Iter end)
{
    for (; begin != end; ++begin)
    {
        if (*begin == ',')
        {
            *begin = '.';
        }
    }
    return begin;
}

template <typename Iter>
Iter fixZerosInTheEnd(Iter begin, Iter end, unsigned int precision)
{
    for (; begin != end; --end)
    {
        if (*(end - 1) != '0')
        {
            return end;
        }
        // Don't delete the last zero before the decimal point.
        if (begin != (end - 1) && begin != (end - 2) && *(end - 2) == '.')
        {
            if (precision)
            {
                return end;
            }
            return end - 2;
        }
    }
    return end;
}

bool IsIntegral(double d)
{
    double integral_part;
    return modf(d, &integral_part) == 0.0;
}

std::string CodePointToUTF8(unsigned int cp)
{
    std::string result;

    if (cp <= 0x7f)
    {
        result.resize(1);
        result[0] = static_cast<char>(cp);
    }
    else if (cp <= 0x7FF)
    {
        result.resize(2);
        result[1] = static_cast<char>(0x80 | (0x3f & cp));
        result[0] = static_cast<char>(0xC0 | (0x1f & (cp >> 6)));
    }
    else if (cp <= 0xFFFF)
    {
        result.resize(3);
        result[2] = static_cast<char>(0x80 | (0x3f & cp));
        result[1] = static_cast<char>(0x80 | (0x3f & (cp >> 6)));
        result[0] = static_cast<char>(0xE0 | (0xf & (cp >> 12)));
    }
    else if (cp <= 0x10FFFF)
    {
        result.resize(4);
        result[3] = static_cast<char>(0x80 | (0x3f & cp));
        result[2] = static_cast<char>(0x80 | (0x3f & (cp >> 6)));
        result[1] = static_cast<char>(0x80 | (0x3f & (cp >> 12)));
        result[0] = static_cast<char>(0xF0 | (0x7 & (cp >> 18)));
    }

    return result;
}

} // namespace

JsonValue::JsonValue(JsonValueType type) :
    m_limit(0),
    m_start(0)
{
    static char const EmptyString[] = "";

    InitBasic(type);
    switch (type)
    {
    case JsonValueType::Null:
        break;
    case JsonValueType::Int:
    case JsonValueType::UnsignedInt:
        m_value.Int = 0;
        break;
    case JsonValueType::Real:
        m_value.Real = 0.0;
        break;
    case JsonValueType::String:
        m_value.String = EmptyString;
        break;
    case JsonValueType::Array:
    case JsonValueType::Object:
        m_value.Map = new ObjectValues();
        break;
    case JsonValueType::Boolean:
        m_value.Bool = false;
        break;
    default:
        assert(false);
    }
}

JsonValue::JsonValue(bool value)
{
    InitBasic(JsonValueType::Boolean);
    m_value.Bool = value;
}

JsonValue::JsonValue(int32_t value)
{
    InitBasic(JsonValueType::Int);
    m_value.Int = value;
}

JsonValue::JsonValue(uint32_t value)
{
    InitBasic(JsonValueType::Int);
    m_value.UInt = value;
}

JsonValue::JsonValue(int64_t value)
{
    InitBasic(JsonValueType::Int);
    m_value.Int = value;
}

JsonValue::JsonValue(uint64_t value)
{
    InitBasic(JsonValueType::Int);
    m_value.UInt = value;
}

JsonValue::JsonValue(double value)
{
    InitBasic(JsonValueType::Real);
    m_value.Real = value;
}

JsonValue::JsonValue(const JsonStaticString & value)
{
    InitBasic(JsonValueType::String);
    m_value.String = const_cast<char *>(value.c_str());
}

JsonValue::JsonValue(const std::string & value)
{
    InitBasic(JsonValueType::String, true);
    m_value.String = DuplicateAndPrefixStringValue(value.data(), static_cast<unsigned>(value.length()));
}

JsonValue::JsonValue(const char * value)
{
    InitBasic(JsonValueType::String, true);
    if (value == nullptr)
    {
        m_value.String = DuplicateAndPrefixStringValue("", 0);
    }
    else
    {
        m_value.String = DuplicateAndPrefixStringValue(value, static_cast<unsigned>(strlen(value)));
    }
}

JsonValue::JsonValue(const char * begin, const char * end)
{
    InitBasic(JsonValueType::String, true);
    m_value.String = DuplicateAndPrefixStringValue(begin, static_cast<unsigned>(end - begin));
}

JsonValue::JsonValue(const JsonValue & other)
{
    DupPayload(other);
    DupMeta(other);
}

JsonValue::JsonValue(JsonValue && other) noexcept
{
    InitBasic(JsonValueType::Null);
    Swap(other);
}

JsonValue::~JsonValue()
{
    ReleasePayload();
    m_value.Int = 0;
}

JsonValue & JsonValue::operator=(const JsonValue & other)
{
    JsonValue(other).Swap(*this);
    return *this;
}

JsonValue & JsonValue::operator=(JsonValue && other) noexcept
{
    other.Swap(*this);
    return *this;
}

void JsonValue::Swap(JsonValue & other)
{
    SwapPayload(other);
    std::swap(m_start, other.m_start);
    std::swap(m_limit, other.m_limit);
}

bool JsonValue::isNull() const
{
    return m_type == JsonValueType::Null;
}

bool JsonValue::isBool() const
{
    return m_type == JsonValueType::Boolean;
}

bool JsonValue::isDouble() const
{
    return m_type == JsonValueType::Int || m_type == JsonValueType::UnsignedInt || m_type == JsonValueType::Real;
}

bool JsonValue::isInt() const
{
    switch (m_type) 
    {
    case JsonValueType::Int:
        return m_value.Int >= MinInt && m_value.Int <= MaxInt;
    case JsonValueType::UnsignedInt:
        return m_value.UInt <= (uint32_t)(MaxInt);
    case JsonValueType::Real:
        return m_value.Real >= MinInt && m_value.Real <= MaxInt && IsIntegral(m_value.Real);
    default:
        break;
    }
    return false;
}

bool JsonValue::isString() const
{
    return m_type == JsonValueType::String;
}

bool JsonValue::isArray() const
{
    return m_type == JsonValueType::Array;
}

bool JsonValue::isObject() const
{
    return Type() == JsonValueType::Object;
}

bool JsonValue::GetString(char const ** begin, char const ** end) const
{
    if (Type() != JsonValueType::String)
    {
        return false;
    }
    if (m_value.String == nullptr)
    {
        return false;
    }
    uint32_t length;
    DecodePrefixedString(m_allocated, m_value.String, &length, begin);
    *end = *begin + length;
    return true;
}

bool JsonValue::asBool() const
{
    switch (m_type)
    {
    case JsonValueType::Boolean: return m_value.Bool;
    case JsonValueType::Null: return false;
    case JsonValueType::Int: return m_value.Int != 0;
    case JsonValueType::UnsignedInt: return m_value.UInt != 0;
    case JsonValueType::Real:
    {
        // According to JavaScript language zero or NaN is regarded as false
        const auto value_classification = std::fpclassify(m_value.Real);
        return value_classification != FP_ZERO && value_classification != FP_NAN;
    }
    default:
        break;
    }
    //JSON_FAIL_MESSAGE("value is not convertible to bool.");
    assert(false);
    return 0;
}

int64_t JsonValue::asInt64() const
{
    switch (Type())
    {
    case JsonValueType::Int: return (int64_t)m_value.Int;
    case JsonValueType::UnsignedInt: return (int64_t)m_value.UInt;
    case JsonValueType::Real: return (int64_t)m_value.Real;
    case JsonValueType::Null: return 0;
    case JsonValueType::Boolean: return m_value.Bool ? 1 : 0;
    }
    assert(false);
    return 0;
}

uint64_t JsonValue::asUInt64() const
{
    switch (Type())
    {
    case JsonValueType::Int: return (uint64_t)m_value.Int;
    case JsonValueType::UnsignedInt: return (uint64_t)m_value.UInt;
    case JsonValueType::Real: return (uint64_t)m_value.Real;
    case JsonValueType::Null: return 0;
    case JsonValueType::Boolean: return m_value.Bool ? 1 : 0;
    }
    assert(false);
    return 0;
}

double JsonValue::asDouble() const
{
    switch (m_type)
    {
    case JsonValueType::Int: return (double)m_value.Int;
    case JsonValueType::UnsignedInt: return (double)m_value.UInt;
    case JsonValueType::Real: return m_value.Real;
    case JsonValueType::Null: return 0.0;
    case JsonValueType::Boolean: return m_value.Bool ? 1.0 : 0.0;
    default:
        break;
    }
    //JSON_FAIL_MESSAGE("value is not convertible to double.");
    assert(false);
    return 0;
}

std::string JsonValue::asString() const
{
    switch (m_type)
    {
    case JsonValueType::Null:
        return "";
    case JsonValueType::String:
    {
        if (m_value.String == nullptr)
        {
            return "";
        }
        unsigned this_len;
        char const * this_str;
        DecodePrefixedString(m_allocated, m_value.String, &this_len, &this_str);
        return std::string(this_str, this_len);
    }
    case JsonValueType::Boolean:
        return m_value.Bool ? "true" : "false";
    case JsonValueType::Int:
        return stdstr_f("%lld", m_value.Int);
    case JsonValueType::UnsignedInt:
        return stdstr_f("%llu", m_value.UInt);
    case JsonValueType::Real:
        return stdstr_f("%f", m_value.Real);
    default:
        assert(false);
        return "";
    }
}

uint32_t JsonValue::size() const
{
    switch (m_type)
    {
    case JsonValueType::Null:
    case JsonValueType::Int:
    case JsonValueType::UnsignedInt:
    case JsonValueType::Real:
    case JsonValueType::Boolean:
    case JsonValueType::String:
        return 0;
    case JsonValueType::Array: // size of the array is highest index + 1
        if (!m_value.Map->empty())
        {
            ObjectValues::const_iterator itLast = m_value.Map->end();
            --itLast;
            return (*itLast).first.Index() + 1;
        }
        return 0;
    case JsonValueType::Object:
        return (uint32_t)(m_value.Map->size());
    }
    return 0;
}

bool JsonValue::empty() const
{
    if (isNull() || isArray() || isObject())
    {
        return size() == 0U;
    }
    return false;
}

JsonValueConstIterator JsonValue::begin() const
{
    switch (Type())
    {
    case JsonValueType::Array:
    case JsonValueType::Object:
        if (m_value.Map)
        {
            return JsonValueConstIterator(m_value.Map->begin());
        }
        break;
    default:
        break;
    }
    return {};
}

JsonValueConstIterator JsonValue::end() const
{
    switch (Type())
    {
    case JsonValueType::Array:
    case JsonValueType::Object:
        if (m_value.Map)
            return JsonValueConstIterator(m_value.Map->end());
        break;
    default:
        break;
    }
    return {};
}

void JsonValue::SetOffsetStart(ptrdiff_t start)
{
    m_start = start;
}

void JsonValue::SetOffsetLimit(ptrdiff_t limit)
{
    m_limit = limit;
}

void JsonValue::SwapPayload(JsonValue & other)
{
    std::swap(m_allocated, other.m_allocated);
    std::swap(m_type, other.m_type);
    std::swap(m_value, other.m_value);
}

JsonValueType JsonValue::Type() const
{
    return m_type;
}

JsonValue & JsonValue::operator[](int32_t Index)
{
    if (Index < 0)
    {
        assert(false);
    }
    return (*this)[(uint32_t)(Index)];
}

JsonValue & JsonValue::operator[](uint32_t Index)
{
    if (Type() == JsonValueType::Null)
    {
        *this = JsonValue(JsonValueType::Array);
    }

    CZString key(Index);
    ObjectValues::iterator it = m_value.Map->lower_bound(key);
    if (it != m_value.Map->end() && (*it).first == key)
    {
        return (*it).second;
    }

    ObjectValues::value_type defaultValue(key, NullSingleton());
    it = m_value.Map->insert(it, defaultValue);
    return (*it).second;
}

JsonValue & JsonValue::operator[](const char * key)
{
    return ResolveReference(key, key + strlen(key));
}

JsonValue & JsonValue::operator[](const std::string & key)
{
    return ResolveReference(key.data(), key.data() + key.length());
}

const JsonValue & JsonValue::operator[](int32_t index) const
{
    return (*this)[(uint32_t)index];
}

const JsonValue & JsonValue::operator[](uint32_t index) const
{
    if (Type() != JsonValueType::Null && Type() != JsonValueType::Array)
    {
        assert(false);
        return NullSingleton();
    }
    if (Type() == JsonValueType::Null)
    {
        return NullSingleton();
    }
    CZString key(index);
    ObjectValues::const_iterator it = m_value.Map->find(key);
    if (it == m_value.Map->end())
    {
        return NullSingleton();
    }
    return (*it).second;
}

const JsonValue & JsonValue::operator[](const char * key) const
{
    JsonValue const * found = Find(key, key + strlen(key));
    if (!found)
    {
        return NullSingleton();
    }
    return *found;
}

JsonValue const & JsonValue::operator[](const std::string & key) const
{
    JsonValue const * found = Find(key.data(), key.data() + key.length());
    if (!found)
    {
        return NullSingleton();
    }
    return *found;
}

JsonValue & JsonValue::Append(const JsonValue & value)
{
    return Append(JsonValue(value));
}

JsonValue & JsonValue::Append(JsonValue && value)
{
    if (Type() == JsonValueType::Null)
    {
        *this = JsonValue(JsonValueType::Array);
    }
    return m_value.Map->emplace(size(), std::move(value)).first->second;
}

void JsonValue::removeMember(const char * key)
{
    assert(Type() == JsonValueType::Null || Type() == JsonValueType::Object);
    if (Type() == JsonValueType::Null)
    {
        return;
    }

    CZString actualKey(key, unsigned(strlen(key)), CZString::NoDuplication);
    m_value.Map->erase(actualKey);
}

bool JsonValue::isMember(char const * key) const
{
    return isMember(key, key + strlen(key));
}

bool JsonValue::isMember(const std::string & key) const
{
    return isMember(key.data(), key.data() + key.length());
}

bool JsonValue::isMember(char const * begin, char const * end) const
{
    JsonValue const * value = Find(begin, end);
    return nullptr != value;
}

JsonValue const * JsonValue::Find(char const * begin) const
{
    return Find(begin, begin + strlen(begin));
}

JsonValue const * JsonValue::Find(char const * begin, char const * end) const
{
    if (Type() == JsonValueType::Null)
    {
        return nullptr;
    }
    CZString actualKey(begin, static_cast<unsigned>(end - begin), CZString::NoDuplication);
    ObjectValues::const_iterator it = m_value.Map->find(actualKey);
    if (it == m_value.Map->end())
    {
        return nullptr;
    }
    return &(*it).second;
}

void JsonValue::InitBasic(JsonValueType type, bool Allocated)
{
    m_type = type;
    m_allocated = Allocated;
    m_start = 0;
    m_limit = 0;
}

void JsonValue::DupPayload(const JsonValue & other)
{
    m_type = other.Type();
    m_allocated = false;
    switch (Type())
    {
    case JsonValueType::Null:
    case JsonValueType::Int:
    case JsonValueType::UnsignedInt:
    case JsonValueType::Real:
    case JsonValueType::Boolean:
        m_value = other.m_value;
        break;
    case JsonValueType::String:
        if (other.m_value.String && other.m_allocated)
        {
            unsigned len;
            char const * str;
            DecodePrefixedString(other.m_allocated, other.m_value.String, &len, &str);
            m_value.String = DuplicateAndPrefixStringValue(str, len);
            m_allocated = true;
        }
        else
        {
            m_value.String = other.m_value.String;
        }
        break;
    case JsonValueType::Array:
    case JsonValueType::Object:
        m_value.Map = new ObjectValues(*other.m_value.Map);
        break;
    }
}

void JsonValue::ReleasePayload()
{
    if (m_type == JsonValueType::String && m_allocated)
    {
        free((void *)m_value.String);
        m_value.String = nullptr;
        m_allocated = false;
    }
    else if ((m_type == JsonValueType::Array || m_type == JsonValueType::Object) && m_value.Map != nullptr)
    {
        delete m_value.Map;
        m_value.Map = nullptr;
    }
}

void JsonValue::DupMeta(const JsonValue & other)
{
    m_start = other.m_start;
    m_limit = other.m_limit;
}

JsonValue & JsonValue::ResolveReference(const char * key)
{
    if (Type() == JsonValueType::Null)
    {
        *this = JsonValue(JsonValueType::Object);
    }
    CZString actualKey(key, static_cast<unsigned>(strlen(key)), CZString::NoDuplication);
    auto it = m_value.Map->lower_bound(actualKey);
    if (it != m_value.Map->end() && (*it).first == actualKey)
    {
        return (*it).second;
    }

    ObjectValues::value_type defaultValue(actualKey, NullSingleton());
    it = m_value.Map->insert(it, defaultValue);
    JsonValue & value = (*it).second;
    return value;
}

// @param key is not null-terminated.
JsonValue & JsonValue::ResolveReference(char const * key, char const * end)
{
    if (Type() == JsonValueType::Null)
    {
        *this = JsonValue(JsonValueType::Object);
    }
    CZString actualKey(key, static_cast<unsigned>(end - key), CZString::DuplicateOnCopy);
    auto it = m_value.Map->lower_bound(actualKey);
    if (it != m_value.Map->end() && (*it).first == actualKey)
    {
        return (*it).second;
    }

    ObjectValues::value_type defaultValue(actualKey, NullSingleton());
    it = m_value.Map->insert(it, defaultValue);
    JsonValue & value = (*it).second;
    return value;
}

JsonMembers JsonValue::GetMemberNames() const
{
    if (Type() == JsonValueType::Null)
    {
        return JsonMembers();
    }
    JsonMembers members;
    members.reserve(m_value.Map->size());
    ObjectValues::const_iterator it = m_value.Map->begin();
    ObjectValues::const_iterator itEnd = m_value.Map->end();
    for (; it != itEnd; ++it)
    {
        members.push_back(std::string((*it).first.Data(), (*it).first.Length()));
    }
    return members;
}

char * JsonValue::DuplicateAndPrefixStringValue(const char * value, uint32_t length)
{
    if (length > static_cast<unsigned>(JsonValue::MaxInt) - sizeof(uint32_t) - 1U)
    {
        return nullptr;
    }
    size_t ActualLength = sizeof(length) + length + 1;
    char * NewString = (char *)(malloc(ActualLength));
    if (NewString == nullptr)
    {
        return nullptr;
    }
    *((uint32_t *)NewString) = length;
    memcpy(NewString + sizeof(uint32_t), value, length);
    NewString[ActualLength - 1U] = 0;
    return NewString;
}

void JsonValue::DecodePrefixedString(bool isPrefixed, char const * Prefixed, uint32_t * length, char const ** value)
{
    if (!isPrefixed)
    {
        *length = static_cast<uint32_t>(strlen(Prefixed));
        *value = Prefixed;
    }
    else
    {
        *length = *reinterpret_cast<unsigned const *>(Prefixed);
        *value = Prefixed + sizeof(uint32_t);
    }
}

JsonValue const & JsonValue::NullSingleton()
{
    static JsonValue const NullStatic;
    return NullStatic;
}

JsonValue::CZString::CZString(uint32_t Index) :
    m_cstr(nullptr),
    m_index(Index)
{
}

JsonValue::CZString::CZString(char const * Str, unsigned length, DuplicationPolicy allocate) :
    m_cstr(Str)
{
    // allocate != duplicate
    m_storage.policy = allocate & 0x3;
    m_storage.length = length & 0x3FFFFFFF;
}

JsonValue::CZString::CZString(CZString const & other)
{
    m_cstr = (other.m_storage.policy != NoDuplication && other.m_cstr != nullptr ? DuplicateStringValue(other.m_cstr, other.m_storage.length) : other.m_cstr);
    m_storage.policy = static_cast<unsigned>(other.m_cstr ? (static_cast<DuplicationPolicy>(other.m_storage.policy) == NoDuplication ? NoDuplication : Duplicate) : static_cast<DuplicationPolicy>(other.m_storage.policy)) & 3U;
    m_storage.length = other.m_storage.length;
}

JsonValue::CZString::~CZString()
{
    if (m_cstr != nullptr && m_storage.policy == Duplicate)
    {
        ReleaseStringValue(m_cstr, m_storage.length + 1U);
    }
}

bool JsonValue::CZString::operator<(const CZString & other) const
{
    if (m_cstr == nullptr)
    {
        return m_index < other.m_index;
    }
    unsigned this_len = this->m_storage.length;
    unsigned other_len = other.m_storage.length;
    unsigned min_len = std::min<unsigned>(this_len, other_len);
    int comp = memcmp(this->m_cstr, other.m_cstr, min_len);
    if (comp < 0)
    {
        return true;
    }
    if (comp > 0)
    {
        return false;
    }
    return (this_len < other_len);
}

bool JsonValue::CZString::operator==(const CZString & other) const
{
    if (m_cstr == nullptr)
    {
        return m_index == other.m_index;
    }
    unsigned this_len = this->m_storage.length;
    unsigned other_len = other.m_storage.length;
    if (this_len != other_len)
    {
        return false;
    }
    int comp = memcmp(this->m_cstr, other.m_cstr, this_len);
    return comp == 0;
}

uint32_t JsonValue::CZString::Index() const
{
    return m_index;
}

const char * JsonValue::CZString::Data() const
{
    return m_cstr;
}

unsigned JsonValue::CZString::Length() const
{
    return m_storage.length;
}

bool JsonValue::CZString::isStaticString() const
{
    return m_storage.policy == NoDuplication;
}

char * JsonValue::CZString::DuplicateStringValue(const char * value, size_t length)
{
    if (length >= static_cast<size_t>(JsonValue::MaxInt))
    {
        length = JsonValue::MaxInt - 1;
    }

    char * NewString = static_cast<char *>(malloc(length + 1));
    if (NewString == nullptr)
    {
        return NewString;
    }
    memcpy(NewString, value, length);
    NewString[length] = 0;
    return NewString;
}

void JsonValue::CZString::ReleaseStringValue(const char * value, size_t length)
{
    // length==0 => we allocated the strings memory
    size_t Size = (length == 0) ? strlen(value) : length;
    memset((void *)value, 0, Size);
    free((void *)value);
}

JsonValueIteratorBase::JsonValueIteratorBase() :
    m_isNull(true)
{
}

JsonValueIteratorBase::JsonValueIteratorBase(const JsonValue::ObjectValues::iterator & current) :
    m_current(current),
    m_isNull(false)
{
}

bool JsonValueIteratorBase::operator==(const JsonValueIteratorBase & other) const
{
    return isEqual(other);
}

bool JsonValueIteratorBase::operator!=(const JsonValueIteratorBase & other) const
{
    return !isEqual(other);
}

void JsonValueIteratorBase::Increment()
{
    m_current++;
}

bool JsonValueIteratorBase::isEqual(const JsonValueIteratorBase & other) const
{
    if (m_isNull)
    {
        return other.m_isNull;
    }
    return m_current == other.m_current;
}

JsonValue & JsonValueIteratorBase::deref()
{
    return m_current->second;
}

const JsonValue & JsonValueIteratorBase::deref() const
{
    return m_current->second;
}

JsonValue JsonValueIteratorBase::Key() const
{
    const JsonValue::CZString czstring = (*m_current).first;
    if (czstring.Data())
    {
        if (czstring.isStaticString())
        {
            return JsonValue(JsonStaticString(czstring.Data()));
        }
        return JsonValue(czstring.Data(), czstring.Data() + czstring.Length());
    }
    return JsonValue(czstring.Index());
}

JsonValueConstIterator::JsonValueConstIterator()
{
}

JsonValueConstIterator::JsonValueConstIterator(const JsonValue::ObjectValues::iterator & current) :
    JsonValueIteratorBase(current)
{
}

JsonValueConstIterator JsonValueConstIterator::operator++(int)
{
    JsonValueConstIterator temp(*this);
    ++*this;
    return temp;
}

JsonValueConstIterator & JsonValueConstIterator::operator++()
{
    Increment();
    return *this;
}

const JsonValue & JsonValueConstIterator::operator*() const
{
    return deref();
}

const JsonValue * JsonValueConstIterator::operator->() const
{
    return &deref();
}

bool JsonReader::Parse(const char * beginDoc, const char * endDoc, JsonValue & root)
{
    m_begin = beginDoc;
    m_end = endDoc;
    m_current = m_begin;
    m_errors.clear();
    while (!m_nodes.empty())
    {
        m_nodes.pop();
    }
    m_nodes.push(&root);
    bool successful = ReadValue();
    return successful;
}

bool JsonReader::AddError(const std::string & message, JsonToken & token, const char * extra)
{
    JsonErrorInfo info;
    info.token = token;
    info.message = message;
    info.extra = extra;
    m_errors.push_back(info);
    return false;
}

bool JsonReader::AddErrorAndRecover(const std::string & Message, JsonToken & token, JsonTokenType SkipUntilToken)
{
    AddError(Message, token);
    return RecoverFromError(SkipUntilToken);
}

bool JsonReader::RecoverFromError(JsonTokenType SkipUntilToken)
{
    size_t const errorCount = m_errors.size();
    JsonToken skip;
    for (;;)
    {
        if (!ReadToken(skip))
        {
            m_errors.resize(errorCount); // discard errors caused by recovery
        }
        if (skip.type == SkipUntilToken || skip.type == JsonToken_EndOfStream)
        {
            break;
        }
    }
    m_errors.resize(errorCount);
    return false;
}

bool JsonReader::Match(const char * pattern, int patternLength)
{
    if (m_end - m_current < patternLength)
    {
        return false;
    }
    for (int i = patternLength - 1; i >= 0; i--)
    {
        if (m_current[i] != pattern[i])
        {
            return false;
        }
    }
    m_current += patternLength;
    return true;
}

bool JsonReader::ReadValue()
{
    bool successful = true;
    JsonToken token;
    ReadToken(token);
    switch (token.type)
    {
    case JsonToken_ObjectBegin:
        successful = ReadObject(token);
        CurrentValue().SetOffsetLimit(m_current - m_begin);
        break;
    case JsonToken_ArrayBegin:
        successful = ReadArray(token);
        CurrentValue().SetOffsetLimit(m_current - m_begin);
        break;
    case JsonToken_Number:
        successful = DecodeNumber(token);
        break;
    case JsonToken_String:
        successful = DecodeString(token);
        break;
    case JsonToken_True:
    {
        JsonValue v(true);
        CurrentValue().SwapPayload(v);
        CurrentValue().SetOffsetStart(token.start - m_begin);
        CurrentValue().SetOffsetLimit(token.end - m_begin);
    }
    break;
    case JsonToken_False:
    {
        JsonValue v(false);
        CurrentValue().SwapPayload(v);
        CurrentValue().SetOffsetStart(token.start - m_begin);
        CurrentValue().SetOffsetLimit(token.end - m_begin);
    }
    break;
    default:
#ifdef WIN32
        __debugbreak();
#endif
        return false;
    }
    return successful;
}

bool JsonReader::ReadObject(JsonToken & token)
{
    JsonToken TokenName;
    std::string name;
    JsonValue init(JsonValueType::Object);
    CurrentValue().SwapPayload(init);
    CurrentValue().SetOffsetStart(token.start - m_begin);
    while (ReadToken(TokenName))
    {
        if (TokenName.type == JsonToken_ObjectEnd && name.empty()) // empty object
        {
            return true;
        }
        name.clear();
        if (TokenName.type == JsonToken_String)
        {
            if (!DecodeString(TokenName, name))
            {
                return RecoverFromError(JsonToken_ObjectEnd);
            }
        }
        else
        {
            break;
        }

        JsonToken Colon;
        if (!ReadToken(Colon) || Colon.type != JsonToken_MemberSeparator)
        {
            return AddErrorAndRecover("Missing ':' after object member name", Colon, JsonToken_ObjectEnd);
        }
        JsonValue & value = CurrentValue()[name];
        m_nodes.push(&value);
        bool ok = ReadValue();
        m_nodes.pop();
        if (!ok) // error already set
        {
            return RecoverFromError(JsonToken_ObjectEnd);
        }
        JsonToken Comma;
        if (!ReadToken(Comma) || (Comma.type != JsonToken_ObjectEnd && Comma.type != JsonToken_ArraySeparator && Comma.type != JsonToken_Comment))
        {
            return AddErrorAndRecover("Missing ',' or '}' in object declaration", Comma, JsonToken_ObjectEnd);
        }
        bool finalizeTokenOk = true;
        while (Comma.type == JsonToken_Comment && finalizeTokenOk)
        {
            finalizeTokenOk = ReadToken(Comma);
        }
        if (Comma.type == JsonToken_ObjectEnd)
        {
            return true;
        }
    }
    return AddErrorAndRecover("Missing '}' or object member name", TokenName, JsonToken_ObjectEnd);
}

bool JsonReader::ReadArray(JsonToken & token)
{
    JsonValue init(JsonValueType::Array);
    CurrentValue().SwapPayload(init);
    CurrentValue().SetOffsetStart(token.start - m_begin);
    SkipSpaces();
    if (m_current != m_end && *m_current == ']') // empty array
    {
        JsonToken endArray;
        ReadToken(endArray);
        return true;
    }
    int index = 0;
    for (;;)
    {
        JsonValue & value = CurrentValue()[index++];
        m_nodes.push(&value);
        bool ok = ReadValue();
        m_nodes.pop();
        if (!ok)
        {
            return RecoverFromError(JsonToken_ArrayEnd);
        }
        JsonToken currentToken;
        // Accept Comment after last item in the array.
        ok = ReadToken(currentToken);
        bool badTokenType = (currentToken.type != JsonToken_ArraySeparator && currentToken.type != JsonToken_ArrayEnd);
        if (!ok || badTokenType)
        {
            return AddErrorAndRecover("Missing ',' or ']' in array declaration", currentToken, JsonToken_ArrayEnd);
        }
        if (currentToken.type == JsonToken_ArrayEnd)
        {
            break;
        }
    }
    return true;
}

bool JsonReader::ReadToken(JsonToken & token)
{
    SkipSpaces();
    token.start = m_current;
    char c = GetNextChar();
    bool ok = true;
    switch (c)
    {
    case '{':
        token.type = JsonToken_ObjectBegin;
        break;
    case '}':
        token.type = JsonToken_ObjectEnd;
        break;
    case '[':
        token.type = JsonToken_ArrayBegin;
        break;
    case ']':
        token.type = JsonToken_ArrayEnd;
        break;
    case '"':
        token.type = JsonToken_String;
        ok = ReadString();
        break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
    case '-':
        token.type = JsonToken_Number;
        ReadNumber();
        break;
    case 't':
        token.type = JsonToken_True;
        ok = Match("rue", 3);
        break;
    case 'f':
        token.type = JsonToken_False;
        ok = Match("alse", 4);
        break;
    case ',':
        token.type = JsonToken_ArraySeparator;
        break;
    case ':':
        token.type = JsonToken_MemberSeparator;
        break;
    default:
#ifdef WIN32
        __debugbreak();
#endif
        ok = false;
        break;
    }
    if (!ok)
    {
        token.type = JsonToken_Error;
    }
    token.end = m_current;
    return ok;
}
void JsonReader::ReadNumber()
{
    const char * p = m_current;
    char c = '0';
    while (c >= '0' && c <= '9')
    {
        c = (m_current = p) < m_end ? *p++ : '\0';
    }

    // fractional part
    if (c == '.')
    {
        c = (m_current = p) < m_end ? *p++ : '\0';
        while (c >= '0' && c <= '9')
        {
            c = (m_current = p) < m_end ? *p++ : '\0';
        }
    }

    // exponential part
    if (c == 'e' || c == 'E')
    {
        c = (m_current = p) < m_end ? *p++ : '\0';
        if (c == '+' || c == '-')
        {
            c = (m_current = p) < m_end ? *p++ : '\0';
        }
        while (c >= '0' && c <= '9')
        {
            c = (m_current = p) < m_end ? *p++ : '\0';
        }
    }
}

bool JsonReader::ReadString()
{
    char c = '\0';
    while (m_current != m_end)
    {
        c = GetNextChar();
        if (c == '\\')
        {
            GetNextChar();
        }
        else if (c == '"')
        {
            break;
        }
    }
    return c == '"';
}

bool JsonReader::DecodeDouble(JsonToken & token, JsonValue & Decoded)
{
    double value = 0;
    std::string Buffer(token.start, token.end);
    std::basic_istringstream<std::string::value_type, std::string::traits_type, std::string::allocator_type> IStream(Buffer);
    if (!(IStream >> value))
    {
        if (value == std::numeric_limits<double>::max())
        {
            value = std::numeric_limits<double>::infinity();
        }
        else if (value == std::numeric_limits<double>::lowest())
        {
            value = -std::numeric_limits<double>::infinity();
        }
        else if (!std::isinf(value))
        {
            return AddError("'" + std::string(token.start, token.end) + "' is not a number.", token);
        }
    }
    Decoded = value;
    return true;
}

bool JsonReader::DecodeNumber(JsonToken & token)
{
    JsonValue Decoded;
    if (!DecodeNumber(token, Decoded))
    {
        return false;
    }
    CurrentValue().SwapPayload(Decoded);
    CurrentValue().SetOffsetStart(token.start - m_begin);
    CurrentValue().SetOffsetLimit(token.end - m_begin);
    return true;
}

bool JsonReader::DecodeNumber(JsonToken & token, JsonValue & Decoded)
{
    // Attempts to parse the number as an integer. If the number is
    // larger than the maximum supported value of an integer then
    // we decode the number as a double.
    const char * current = token.start;
    bool isNegative = *current == '-';
    if (isNegative)
    {
        current += 1;
    }
    int64_t MaxIntegerValue = isNegative ? (uint64_t)((int64_t)(uint64_t(-1) / 2)) + 1 : uint64_t(-1);
    int64_t Threshold = MaxIntegerValue / 10ull;
    int64_t value = 0;
    while (current < token.end)
    {
        char c = *current;
        current += 1;
        if (c < '0' || c > '9')
        {
            return DecodeDouble(token, Decoded);
        }
        uint32_t Digit(static_cast<uint32_t>(c - '0'));
        if (value >= Threshold)
        {
            // We've hit or exceeded the max value divided by 10 (rounded down). If
            // a) we've only just touched the limit, b) this is the last digit, and
            // c) it's small enough to fit in that rounding delta, we're okay.
            // Otherwise treat this number as a double to avoid overflow.
            if (value > Threshold || current != token.end || Digit > MaxIntegerValue % 10)
            {
                return DecodeDouble(token, Decoded);
            }
        }
        value = value * 10 + Digit;
    }
    if (isNegative && value == MaxIntegerValue)
    {
        Decoded = JsonValue::MinLargestInt;
    }
    else if (isNegative)
    {
        Decoded = -((int64_t)(value));
    }
    else if (value <= (uint64_t)(JsonValue::MaxInt))
    {
        Decoded = (int64_t)(value);
    }
    else
    {
        Decoded = value;
    }
    return true;
}

bool JsonReader::DecodeString(JsonToken & token)
{
    std::string DecodedString;
    if (!DecodeString(token, DecodedString))
    {
        return false;
    }
    JsonValue Decoded(DecodedString);
    CurrentValue().SwapPayload(Decoded);
    CurrentValue().SetOffsetStart(token.start - m_begin);
    CurrentValue().SetOffsetLimit(token.end - m_begin);
    return true;
}

bool JsonReader::DecodeString(JsonToken & token, std::string & Decoded)
{
    Decoded.reserve((size_t)(token.end - token.start - 2));
    const char * current = token.start + 1; // skip '"'
    const char * end = token.end - 1;       // do not include '"'
    while (current != end)
    {
        char c = *current++;
        if (c == '"')
        {
            break;
        }
        if (c == '\\')
        {
            if (current == end)
            {
                return AddError("Empty escape sequence in string", token, current);
            }
            char Escape = *current++;
            switch (Escape)
            {
            case '"':
                Decoded += '"';
                break;
            case '/':
                Decoded += '/';
                break;
            case '\\':
                Decoded += '\\';
                break;
            case 'b':
                Decoded += '\b';
                break;
            case 'f':
                Decoded += '\f';
                break;
            case 'n':
                Decoded += '\n';
                break;
            case 'r':
                Decoded += '\r';
                break;
            case 't':
                Decoded += '\t';
                break;
            case 'u':
            {
                unsigned int unicode;
                if (!DecodeUnicodeCodePoint(token, current, end, unicode))
                {
                    return false;
                }
                Decoded += CodePointToUTF8(unicode);
                break;
            }
            default:
                return AddError("Bad escape sequence in string", token, current);
            }
        }
        else
        {
            Decoded += c;
        }
    }
    return true;
}

bool JsonReader::DecodeUnicodeCodePoint(JsonToken & token, const char *& current, const char * end, unsigned int & unicode)
{
    if (!DecodeUnicodeEscapeSequence(token, current, end, unicode))
    {
        return false;
    }
    if (unicode >= 0xD800 && unicode <= 0xDBFF)
    {
        if (end - current < 6)
        {
            return AddError("additional six characters expected to parse unicode surrogate pair.", token, current);
        }
        if (*(current++) == '\\' && *(current++) == 'u')
        {
            unsigned int surrogatePair;
            if (DecodeUnicodeEscapeSequence(token, current, end, surrogatePair))
            {
                unicode = 0x10000 + ((unicode & 0x3FF) << 10) + (surrogatePair & 0x3FF);
            }
            else
            {
                return false;
            }
        }
        else
        {
            return AddError("expecting another \\u token to begin the second half of a unicode surrogate pair", token, current);
        }
    }
    return true;
}

bool JsonReader::DecodeUnicodeEscapeSequence(JsonToken & token, const char *& current, const char * end, unsigned int & unicode)
{
    if (end - current < 4)
    {
        return AddError("Bad unicode escape sequence in string: four digits expected.", token, current);
    }
    int value = 0;
    for (int index = 0; index < 4; ++index)
    {
        char c = *current++;
        value *= 16;
        if (c >= '0' && c <= '9')
        {
            value += c - '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
            value += c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'F')
        {
            value += c - 'A' + 10;
        }
        else
        {
            return AddError("Bad unicode escape sequence in string: hexadecimal digit expected.", token, current);
        }
    }
    unicode = (unsigned int)value;
    return true;
}

void JsonReader::SkipSpaces()
{
    while (m_current != m_end)
    {
        const char c = *m_current;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            m_current++;
            continue;
        }
        break;
    }
}

char JsonReader::GetNextChar()
{
    if (m_current == m_end)
    {
        return 0;
    }
    char NextChar = *m_current;
    m_current += 1;
    return NextChar;
}

JsonValue & JsonReader::CurrentValue()
{
    return *(m_nodes.top());
}

JsonStyledWriter::JsonStyledWriter() :
    m_indentSize(3),
    m_rightMargin(74),
    m_addChildValues(false)
{
}

std::string JsonStyledWriter::write(const JsonValue & root)
{
    m_document.clear();
    m_addChildValues = false;
    m_indentString.clear();
    WriteValue(root);
    m_document += '\n';
    return m_document;
}

void JsonStyledWriter::WriteValue(const JsonValue & value)
{
    switch (value.Type())
    {
    case JsonValueType::Null:
        PushValue("null");
        break;
    case JsonValueType::Int:
        PushValue(valueToString(value.asInt64()));
        break;
    case JsonValueType::UnsignedInt:
        PushValue(valueToString(value.asUInt64()));
        break;
    case JsonValueType::Real:
        PushValue(valueToString(value.asDouble()));
        break;
    case JsonValueType::String:
    {
        // Is NULL possible for value.string_? No.
        char const *str, *end;
        bool ok = value.GetString(&str, &end);
        if (ok)
            PushValue(valueToQuotedStringN(str, static_cast<size_t>(end - str)));
        else
            PushValue("");
        break;
    }
    case JsonValueType::Boolean:
        PushValue(valueToString(value.asBool()));
        break;
    case JsonValueType::Array:
        WriteArrayValue(value);
        break;
    case JsonValueType::Object:
    {
        JsonMembers members(value.GetMemberNames());
        if (members.empty())
        {
            PushValue("{}");
        }
        else
        {
            WriteWithIndent("{");
            Indent();
            auto it = members.begin();
            for (;;)
            {
                const std::string & name = *it;
                const JsonValue & childValue = value[name];
                WriteWithIndent(valueToQuotedString(name.c_str()));
                m_document += " : ";
                WriteValue(childValue);
                if (++it == members.end())
                {
                    break;
                }
                m_document += ',';
            }
            Unindent();
            WriteWithIndent("}");
        }
    }
    break;
    }
}

void JsonStyledWriter::WriteArrayValue(const JsonValue & value)
{
    size_t size = value.size();
    if (size == 0)
    {
        PushValue("[]");
    }
    else
    {
        bool isArrayMultiLine = IsMultilineArray(value);
        if (isArrayMultiLine)
        {
            WriteWithIndent("[");
            Indent();
            bool hasChildValue = !m_childValues.empty();
            uint32_t index = 0;
            for (;;)
            {
                const JsonValue & childValue = value[index];
                if (hasChildValue)
                {
                    WriteWithIndent(m_childValues[index]);
                }
                else
                {
                    WriteIndent();
                    WriteValue(childValue);
                }
                if (++index == size)
                {
                    break;
                }
                m_document += ',';
            }
            Unindent();
            WriteWithIndent("]");
        }
        else // output on a single line
        {
            assert(m_childValues.size() == size);
            m_document += "[ ";
            for (size_t index = 0; index < size; ++index)
            {
                if (index > 0)
                {
                    m_document += ", ";
                }
                m_document += m_childValues[index];
            }
            m_document += " ]";
        }
    }
}

bool JsonStyledWriter::IsMultilineArray(const JsonValue & value)
{
    uint32_t const size = value.size();
    bool isMultiLine = size * 3 >= m_rightMargin;
    m_childValues.clear();
    for (uint32_t index = 0; index < size && !isMultiLine; ++index)
    {
        const JsonValue & childValue = value[index];
        isMultiLine = ((childValue.isArray() || childValue.isObject()) && !childValue.empty());
    }
    if (!isMultiLine) // check if line length > max line length
    {
        m_childValues.reserve(size);
        m_addChildValues = true;
        uint32_t lineLength = 4 + (size - 1) * 2; // '[ ' + ', '*n + ' ]'
        for (uint32_t index = 0; index < size; ++index)
        {
            WriteValue(value[index]);
            lineLength += static_cast<uint32_t>(m_childValues[index].length());
        }
        m_addChildValues = false;
        isMultiLine = isMultiLine || lineLength >= m_rightMargin;
    }
    return isMultiLine;
}

void JsonStyledWriter::PushValue(const std::string & value)
{
    if (m_addChildValues)
    {
        m_childValues.push_back(value);
    }
    else
    {
        m_document += value;
    }
}

void JsonStyledWriter::WriteIndent()
{
    if (!m_document.empty())
    {
        char last = m_document[m_document.length() - 1];
        if (last == ' ') // already indented
        {
            return;
        }
        if (last != '\n') // Comments may add new-line
        {
            m_document += '\n';
        }
    }
    m_document += m_indentString;
}

void JsonStyledWriter::WriteWithIndent(const std::string & value)
{
    WriteIndent();
    m_document += value;
}

void JsonStyledWriter::Indent()
{
    m_indentString += std::string(m_indentSize, ' ');
}

void JsonStyledWriter::Unindent()
{
    assert(m_indentString.size() >= m_indentSize);
    m_indentString.resize(m_indentString.size() - m_indentSize);
}

void JsonStyledWriter::AppendHex(std::string & result, unsigned ch)
{
    result.append("\\u").append(ToHex16Bit(ch));
}

void JsonStyledWriter::AppendRaw(std::string & result, unsigned ch)
{
    result += static_cast<char>(ch);
}

bool JsonStyledWriter::DoesAnyCharRequireEscaping(char const * s, size_t n)
{
    assert(s || !n);

    return std::any_of(s, s + n, [](unsigned char c) { return c == '\\' || c == '"' || c < 0x20 || c > 0x7F; });
}

uint32_t JsonStyledWriter::Utf8ToCodepoint(const char *& s, const char * e)
{
    const unsigned int REPLACEMENT_CHARACTER = 0xFFFD;

    unsigned int firstByte = static_cast<unsigned char>(*s);

    if (firstByte < 0x80)
        return firstByte;

    if (firstByte < 0xE0)
    {
        if (e - s < 2)
            return REPLACEMENT_CHARACTER;

        unsigned int calculated =
            ((firstByte & 0x1F) << 6) | (static_cast<unsigned int>(s[1]) & 0x3F);
        s += 1;
        // oversized encoded characters are invalid
        return calculated < 0x80 ? REPLACEMENT_CHARACTER : calculated;
    }

    if (firstByte < 0xF0)
    {
        if (e - s < 3)
            return REPLACEMENT_CHARACTER;

        unsigned int calculated = ((firstByte & 0x0F) << 12) |
                                  ((static_cast<unsigned int>(s[1]) & 0x3F) << 6) |
                                  (static_cast<unsigned int>(s[2]) & 0x3F);
        s += 2;
        // surrogates aren't valid codepoints itself
        // shouldn't be UTF-8 encoded
        if (calculated >= 0xD800 && calculated <= 0xDFFF)
            return REPLACEMENT_CHARACTER;
        // oversized encoded characters are invalid
        return calculated < 0x800 ? REPLACEMENT_CHARACTER : calculated;
    }

    if (firstByte < 0xF8)
    {
        if (e - s < 4)
            return REPLACEMENT_CHARACTER;

        unsigned int calculated = ((firstByte & 0x07) << 18) |
                                  ((static_cast<unsigned int>(s[1]) & 0x3F) << 12) |
                                  ((static_cast<unsigned int>(s[2]) & 0x3F) << 6) |
                                  (static_cast<unsigned int>(s[3]) & 0x3F);
        s += 3;
        // oversized encoded characters are invalid
        return calculated < 0x10000 ? REPLACEMENT_CHARACTER : calculated;
    }

    return REPLACEMENT_CHARACTER;
}

std::string JsonStyledWriter::ToHex16Bit(unsigned int x)
{
    static const char hex2[] = "000102030405060708090a0b0c0d0e0f"
                               "101112131415161718191a1b1c1d1e1f"
                               "202122232425262728292a2b2c2d2e2f"
                               "303132333435363738393a3b3c3d3e3f"
                               "404142434445464748494a4b4c4d4e4f"
                               "505152535455565758595a5b5c5d5e5f"
                               "606162636465666768696a6b6c6d6e6f"
                               "707172737475767778797a7b7c7d7e7f"
                               "808182838485868788898a8b8c8d8e8f"
                               "909192939495969798999a9b9c9d9e9f"
                               "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
                               "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
                               "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
                               "d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
                               "e0e1e2e3e4e5e6e7e8e9eaebecedeeef"
                               "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";

    const unsigned int hi = (x >> 8) & 0xff;
    const unsigned int lo = x & 0xff;
    std::string result(4, ' ');
    result[0] = hex2[2 * hi];
    result[1] = hex2[2 * hi + 1];
    result[2] = hex2[2 * lo];
    result[3] = hex2[2 * lo + 1];
    return result;
}

std::string JsonStyledWriter::valueToString(int64_t value)
{
    char buffer[3 * sizeof(value) + 1];
    char * current = buffer + sizeof(buffer);
    if (value == JsonValue::MinLargestInt)
    {
        uintToString((uint64_t)(JsonValue::MaxLargestInt) + 1, current);
        *--current = '-';
    }
    else if (value < 0)
    {
        uintToString(((uint64_t)-value), current);
        *--current = '-';
    }
    else
    {
        uintToString((uint64_t)value, current);
    }
    return current;
}

std::string JsonStyledWriter::valueToString(uint64_t value)
{
    char buffer[3 * sizeof(value) + 1];
    char * current = buffer + sizeof(buffer);
    uintToString(value, current);
    assert(current >= buffer);
    return current;
}

std::string JsonStyledWriter::valueToString(bool value)
{
    return value ? "true" : "false";
}

std::string JsonStyledWriter::valueToString(double value, uint32_t precision, JsonPrecisionType precisionType)
{
    return valueToString(value, false, precision, precisionType);
}

std::string JsonStyledWriter::valueToString(double value, bool useSpecialFloats, uint32_t precision, JsonPrecisionType precisionType)
{
    // Print into the buffer. We need not request the alternative representation
    // that always has a decimal point because JSON doesn't distinguish the
    // concepts of reals and integers.
    if (!std::isfinite(value))
    {
        static const char * const reps[2][3] = {{"NaN", "-Infinity", "Infinity"},
                                                {"null", "-1e+9999", "1e+9999"}};
        return reps[useSpecialFloats ? 0 : 1]
                   [std::isnan(value) ? 0 : (value < 0) ? 1
                                                        : 2];
    }

    std::string buffer(size_t(36), '\0');
    while (true)
    {
        uint32_t len = (uint32_t)stdstr_f((precisionType == JsonPrecisionType::significantDigits) ? "%.*g" : "%.*f", precision, value).length();
        assert(len >= 0);
        auto wouldPrint = static_cast<size_t>(len);
        if (wouldPrint >= buffer.size())
        {
            buffer.resize(wouldPrint + 1);
            continue;
        }
        buffer.resize(wouldPrint);
        break;
    }

    buffer.erase(fixNumericLocale(buffer.begin(), buffer.end()), buffer.end());

    // try to ensure we preserve the fact that this was given to us as a double on
    // input
    if (buffer.find('.') == buffer.npos && buffer.find('e') == buffer.npos)
    {
        buffer += ".0";
    }

    // strip the zero padding from the right
    if (precisionType == JsonPrecisionType::decimalPlaces)
    {
        buffer.erase(fixZerosInTheEnd(buffer.begin(), buffer.end(), precision), buffer.end());
    }
    return buffer;
}

std::string JsonStyledWriter::valueToQuotedString(const char * value)
{
    return valueToQuotedStringN(value, strlen(value));
}

std::string JsonStyledWriter::valueToQuotedStringN(const char * value, size_t length, bool emitUTF8)
{
    if (value == nullptr)
    {
        return "";
    }

    if (!DoesAnyCharRequireEscaping(value, length))
    {
        return std::string("\"") + value + "\"";
    }
    // We have to walk value and escape any special characters.
    // Appending to std::string is not efficient, but this should be rare.
    // (Note: forward slashes are *not* rare, but I am not escaping them.)
    std::string::size_type maxsize = length * 2 + 3; // allescaped+quotes+NULL
    std::string result;
    result.reserve(maxsize); // to avoid lots of mallocs
    result += "\"";
    char const * end = value + length;
    for (const char * c = value; c != end; ++c)
    {
        switch (*c)
        {
        case '\"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
            // case '/':
            // Even though \/ is considered a legal escape in JSON, a bare
            // slash is also legal, so I see no reason to escape it.
            // (I hope I am not misunderstanding something.)
            // blep notes: actually escaping \/ may be useful in javascript to avoid </
            // sequence.
            // Should add a flag to allow this compatibility mode and prevent this
            // sequence from occurring.
        default:
        {
            if (emitUTF8)
            {
                unsigned codepoint = static_cast<unsigned char>(*c);
                if (codepoint < 0x20)
                {
                    AppendHex(result, codepoint);
                }
                else
                {
                    AppendRaw(result, codepoint);
                }
            }
            else
            {
                unsigned codepoint = Utf8ToCodepoint(c, end); // modifies `c`
                if (codepoint < 0x20)
                {
                    AppendHex(result, codepoint);
                }
                else if (codepoint < 0x80)
                {
                    AppendRaw(result, codepoint);
                }
                else if (codepoint < 0x10000)
                {
                    // Basic Multilingual Plane
                    AppendHex(result, codepoint);
                }
                else
                {
                    // Extended Unicode. Encode 20 bits as a surrogate pair.
                    codepoint -= 0x10000;
                    AppendHex(result, 0xd800 + ((codepoint >> 10) & 0x3ff));
                    AppendHex(result, 0xdc00 + (codepoint & 0x3ff));
                }
            }
        }
        break;
        }
    }
    result += "\"";
    return result;
}

void JsonStyledWriter::uintToString(uint64_t value, char *& current)
{
    *--current = 0;
    do
    {
        *--current = (char)(value % 10U + (uint32_t)'0');
        value /= 10;
    } while (value != 0);
}
