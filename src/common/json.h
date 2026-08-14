#pragma once
#include <deque>
#include <map>
#include <stack>
#include <stdint.h>
#include <string>
#include <vector>

enum class JsonValueType
{
    Null = 0,
    Int,
    UnsignedInt,
    Real,
    String,
    Boolean,
    Array,
    Object,
};

enum class JsonPrecisionType
{
    significantDigits = 0, ///< we set max number of significant digits in string
    decimalPlaces          ///< we set max number of digits after "." in string
};

class JsonStaticString
{
public:
    explicit JsonStaticString(const char * czstring) :
        c_str_(czstring)
    {
    }

    operator const char *() const
    {
        return c_str_;
    }

    const char * c_str() const
    {
        return c_str_;
    }

private:
    const char * c_str_;
};

typedef std::vector<std::string> JsonMembers;
class JsonValueIterator;
class JsonValueConstIterator;

class JsonValue
{
    friend class JsonValueIteratorBase;
    friend class JsonValueIterator;
    friend class JsonValueConstIterator;

    class CZString
    {
    public:
        enum DuplicationPolicy
        {
            NoDuplication = 0,
            Duplicate,
            DuplicateOnCopy
        };

        CZString(uint32_t index);
        CZString(char const * str, unsigned length, DuplicationPolicy allocate);
        CZString(CZString const & other);
        ~CZString();

        bool operator<(CZString const & other) const;
        bool operator==(CZString const & other) const;
        uint32_t Index() const;
        char const * Data() const;
        unsigned Length() const;
        bool isStaticString() const;

    private:
        static char * DuplicateStringValue(const char * Value, size_t Length);
        static void ReleaseStringValue(const char * Value, size_t Length);

        struct StringStorage
        {
            unsigned policy : 2;
            unsigned length : 30; // 1GB max
        };

        char const * m_cstr; // actually, a prefixed string, unless policy is noDup
        union
        {
            uint32_t m_index;
            StringStorage m_storage;
        };
    };
    typedef std::map<CZString, JsonValue> ObjectValues;

public:
    static constexpr int64_t MinLargestInt = int64_t(~(uint64_t(-1) / 2));
    static constexpr int64_t MaxLargestInt = int64_t(int64_t(-1) / 2);
    static constexpr int32_t MinInt = int32_t(~(uint32_t(-1) / 2));
    static constexpr int32_t MaxInt = int32_t(uint32_t(-1) / 2);
    static constexpr uint32_t DefaultRealPrecision = 17;

    JsonValue(JsonValueType type = JsonValueType::Null);
    JsonValue(bool value);
    JsonValue(int32_t value);
    JsonValue(uint32_t value);
    JsonValue(int64_t value);
    JsonValue(uint64_t value);
    JsonValue(double value);
    JsonValue(const JsonStaticString & value);
    JsonValue(const std::string & value);
    JsonValue(const char * value);
    JsonValue(const char * begin, const char * end);
    JsonValue(const JsonValue & other);
    JsonValue(JsonValue && other) noexcept;
    ~JsonValue();

    JsonValue & operator=(const JsonValue & other);
    JsonValue & operator=(JsonValue && other) noexcept;

    void Swap(JsonValue & other);

    bool isNull() const;
    bool isBool() const;
    bool isDouble() const;
    bool isInt() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;

    bool GetString(char const ** begin, char const ** end) const;
    bool asBool() const;
    int64_t asInt64() const;
    uint64_t asUInt64() const;
    double asDouble() const;
    std::string asString() const;

    uint32_t size() const;
    bool empty() const;

    JsonValueIterator begin();
    JsonValueIterator end();
    JsonValueConstIterator begin() const;
    JsonValueConstIterator end() const;

    void SetOffsetStart(ptrdiff_t start);
    void SetOffsetLimit(ptrdiff_t limit);
    void SwapPayload(JsonValue & other);

    JsonValueType Type() const;

    JsonValue & operator[](int32_t index);
    JsonValue & operator[](uint32_t index);
    JsonValue & operator[](const char * key);
    JsonValue & operator[](const std::string & key);

    const JsonValue & operator[](int32_t index) const;
    const JsonValue & operator[](uint32_t index) const;
    const JsonValue & operator[](const char * key) const;
    const JsonValue & operator[](const std::string & key) const;

    JsonValue & Append(const JsonValue & value);
    JsonValue & Append(JsonValue && value);
    void removeMember(const char * key);

    bool isMember(const char * key) const;
    bool isMember(const std::string & key) const;
    bool isMember(const char * begin, const char * end) const;

    JsonValue const * Find(char const * begin) const;
    JsonValue const * Find(char const * begin, char const * end) const;
    JsonValue & ResolveReference(const char * key);
    JsonValue & ResolveReference(const char * key, const char * end);

    JsonMembers GetMemberNames() const;

private:
    void InitBasic(JsonValueType type, bool allocated = false);
    void DupPayload(const JsonValue & other);
    void ReleasePayload();
    void DupMeta(const JsonValue & other);

    static char * DuplicateAndPrefixStringValue(const char * value, uint32_t length);
    static void DecodePrefixedString(bool isPrefixed, char const * prefixed, uint32_t * length, char const ** value);

    static JsonValue const & NullSingleton();

    struct StringValue
    {
        uint32_t len;
        char string[1];
    };
    union
    {
        const char * String;
        const StringValue * StrValue;
        bool Bool;
        int64_t Int;
        uint64_t UInt;
        double Real;
        ObjectValues * Map;
    } m_value;
    bool m_allocated;
    JsonValueType m_type;
    ptrdiff_t m_limit;
    ptrdiff_t m_start;
};

class JsonValueIteratorBase
{
protected:
    JsonValueIteratorBase();
    explicit JsonValueIteratorBase(const JsonValue::ObjectValues::iterator & Current);

    void Increment();
    bool isEqual(const JsonValueIteratorBase & other) const;
    const JsonValue & deref() const;
    JsonValue & deref();

public:
    bool operator==(const JsonValueIteratorBase & Other) const;
    bool operator!=(const JsonValueIteratorBase & Other) const;

    JsonValue Key() const;

private:
    JsonValue::ObjectValues::iterator m_current;
    bool m_isNull;
};

class JsonValueIterator :
    public JsonValueIteratorBase
{
    friend class JsonValue;
};

class JsonValueConstIterator :
    public JsonValueIteratorBase
{
    friend class JsonValue;

public:
    JsonValueConstIterator();

    JsonValueConstIterator operator++(int);
    JsonValueConstIterator & operator++();
    const JsonValue & operator*() const;
    const JsonValue * operator->() const;

private:
    explicit JsonValueConstIterator(const JsonValue::ObjectValues::iterator & current);
};

class JsonReader
{
public:
    bool Parse(const char * beginDoc, const char * endDoc, JsonValue & root);

private:
    enum JsonTokenType
    {
        JsonToken_EndOfStream = 0,
        JsonToken_ObjectBegin,
        JsonToken_ObjectEnd,
        JsonToken_ArrayBegin,
        JsonToken_ArrayEnd,
        JsonToken_String,
        JsonToken_Number,
        JsonToken_True,
        JsonToken_False,
        JsonToken_Null,
        JsonToken_ArraySeparator,
        JsonToken_MemberSeparator,
        JsonToken_Comment,
        JsonToken_Error
    };

    struct JsonToken
    {
        JsonTokenType type;
        const char * start;
        const char * end;
    };

    struct JsonErrorInfo
    {
        JsonToken token;
        std::string message;
        const char * extra;
    };
    typedef std::deque<JsonErrorInfo> JsonErrors;

    bool AddError(const std::string & message, JsonToken & token, const char * extra = nullptr);
    bool AddErrorAndRecover(const std::string & message, JsonToken & token, JsonTokenType skipUntilToken);
    bool RecoverFromError(JsonTokenType skipUntilToken);
    bool Match(const char * pattern, int patternLength);
    bool ReadValue();
    bool ReadObject(JsonToken & token);
    bool ReadArray(JsonToken & token);
    bool ReadToken(JsonToken & token);
    void ReadNumber();
    bool ReadString();
    bool DecodeDouble(JsonToken & token);
    bool DecodeDouble(JsonToken & token, JsonValue & decoded);
    bool DecodeNumber(JsonToken & token);
    bool DecodeNumber(JsonToken & token, JsonValue & decoded);
    bool DecodeString(JsonToken & token);
    bool DecodeString(JsonToken & token, std::string & decoded);
    bool DecodeUnicodeCodePoint(JsonToken & token, const char *& current, const char * end, unsigned int & unicode);
    bool DecodeUnicodeEscapeSequence(JsonToken & token, const char *& current, const char * end, unsigned int & unicode);
    void SkipSpaces();
    char GetNextChar();
    JsonValue & CurrentValue();

    JsonErrors m_errors;
    const char * m_begin;
    const char * m_end;
    const char * m_current;
    std::stack<JsonValue *> m_nodes;
};

class JsonStyledWriter
{
    typedef std::vector<std::string> ChildValues;

public:
    JsonStyledWriter();
    ~JsonStyledWriter() = default;

    std::string write(const JsonValue & root);

private:
    void WriteValue(const JsonValue & value);
    void WriteArrayValue(const JsonValue & value);
    bool IsMultilineArray(const JsonValue & value);
    void PushValue(const std::string & value);
    void WriteIndent();
    void WriteWithIndent(const std::string & value);
    void Indent();
    void Unindent();

    static void AppendHex(std::string & result, unsigned ch);
    static void AppendRaw(std::string & result, unsigned ch);
    static bool DoesAnyCharRequireEscaping(char const * s, size_t n);
    static uint32_t Utf8ToCodepoint(const char *& s, const char * e);
    static std::string ToHex16Bit(uint32_t x);
    static std::string valueToString(int64_t value);
    static std::string valueToString(uint64_t value);
    static std::string valueToString(bool value);
    static std::string valueToString(double value, uint32_t precision = JsonValue::DefaultRealPrecision, JsonPrecisionType precisionType = JsonPrecisionType::significantDigits);
    static std::string valueToString(double value, bool useSpecialFloats, uint32_t precision, JsonPrecisionType precisionType);
    static std::string valueToQuotedString(const char * value);
    static std::string valueToQuotedStringN(const char * value, size_t length, bool emitUTF8 = false);

    static void uintToString(uint64_t value, char *& current);

    ChildValues m_childValues;
    std::string m_document;
    std::string m_indentString;
    uint32_t m_rightMargin;
    uint32_t m_indentSize;
    bool m_addChildValues;
};