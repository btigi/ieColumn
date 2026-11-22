#include "BinaryReader.h"

#include "Tlk.h"

#include <strsafe.h>

#include <cstring>

namespace
{
    struct RawValue
    {
        bool isText = false;
        bool isSigned = false;
        int64_t i = 0;
        uint64_t u = 0;
        std::wstring text;
    };

    size_t ScalarWidth(ScalarType type)
    {
        switch (type)
        {
        case ScalarType::U8:
        case ScalarType::I8:
            return 1;
        case ScalarType::U16:
        case ScalarType::I16:
            return 2;
        case ScalarType::U32:
        case ScalarType::I32:
            return 4;
        case ScalarType::Resref:
            return 8;
        }
        return 0;
    }

    bool AddOffset(uint64_t a, uint64_t b, uint64_t& out)
    {
        if (a > UINT64_MAX - b)
        {
            return false;
        }
        out = a + b;
        return true;
    }

    bool ReadScalar(const uint8_t* data, size_t size, uint64_t offset, ScalarType type, RawValue& out)
    {
        const size_t width = ScalarWidth(type);
        if (width == 0 || offset > size || width > size - static_cast<size_t>(offset))
        {
            return false;
        }

        const uint8_t* p = data + static_cast<size_t>(offset);
        out = {};
        switch (type)
        {
        case ScalarType::U8:
            out.u = p[0];
            out.i = static_cast<int64_t>(out.u);
            break;
        case ScalarType::I8:
            out.u = p[0];
            out.i = static_cast<int8_t>(p[0]);
            out.isSigned = true;
            break;
        case ScalarType::U16:
        {
            uint16_t v = 0;
            memcpy(&v, p, sizeof(v));
            out.u = v;
            out.i = static_cast<int64_t>(v);
            break;
        }
        case ScalarType::I16:
        {
            uint16_t bits = 0;
            memcpy(&bits, p, sizeof(bits));
            int16_t v = 0;
            memcpy(&v, p, sizeof(v));
            out.u = bits;
            out.i = v;
            out.isSigned = true;
            break;
        }
        case ScalarType::U32:
        {
            uint32_t v = 0;
            memcpy(&v, p, sizeof(v));
            out.u = v;
            out.i = static_cast<int64_t>(v);
            break;
        }
        case ScalarType::I32:
        {
            uint32_t bits = 0;
            memcpy(&bits, p, sizeof(bits));
            int32_t v = 0;
            memcpy(&v, p, sizeof(v));
            out.u = bits;
            out.i = v;
            out.isSigned = true;
            break;
        }
        case ScalarType::Resref:
        {
            size_t n = 0;
            while (n < 8 && p[n] != 0)
            {
                ++n;
            }
            while (n > 0 && p[n - 1] == ' ')
            {
                --n;
            }
            out.isText = true;
            out.text.assign(n, L'\0');
            for (size_t i = 0; i < n; ++i)
            {
                out.text[i] = static_cast<unsigned char>(p[i]);
            }
            break;
        }
        }
        return true;
    }

    std::wstring FormatNumber(const RawValue& value)
    {
        wchar_t buf[64] = {};
        if (value.isSigned)
        {
            StringCchPrintfW(buf, ARRAYSIZE(buf), L"%lld", static_cast<long long>(value.i));
        }
        else
        {
            StringCchPrintfW(buf, ARRAYSIZE(buf), L"%llu", static_cast<unsigned long long>(value.u));
        }
        return buf;
    }

    bool Locate(const uint8_t* data, size_t size, const Rule& rule, uint64_t& address)
    {
        if (!rule.indirect)
        {
            address = rule.offset;
            return true;
        }

        RawValue base;
        if (!ReadScalar(data, size, rule.baseOffset, ScalarType::U32, base))
        {
            return false;
        }
        if (rule.index != 0 && rule.stride > UINT64_MAX / rule.index)
        {
            return false;
        }

        uint64_t scaled = rule.index * rule.stride;
        uint64_t withBase = 0;
        if (!AddOffset(base.u, scaled, withBase) || !AddOffset(withBase, rule.fieldOffset, address))
        {
            return false;
        }
        return true;
    }

    bool EvalRule(const uint8_t* data, size_t size, const Rule& rule, RawValue& out);

    bool EvalFlags(const uint8_t* data, size_t size, const Rule& rule, RawValue& out)
    {
        uint64_t address = 0;
        RawValue raw;
        if (!Locate(data, size, rule, address) || !ReadScalar(data, size, address, rule.type, raw))
        {
            return false;
        }

        const size_t widthBits = ScalarWidth(rule.type) * 8;
        std::wstring text;
        for (const auto& entry : rule.bitLabels)
        {
            if (entry.first >= widthBits)
            {
                continue;
            }
            if (((raw.u >> entry.first) & 1ull) == 0)
            {
                continue;
            }
            if (!text.empty())
            {
                text += L", ";
            }
            text += entry.second;
        }

        out = {};
        out.isText = true;
        out.text = text;
        return true;
    }

    bool EvalFormat(const uint8_t* data, size_t size, const Rule& rule, RawValue& out)
    {
        std::map<std::wstring, std::wstring> values;
        for (const auto& field : rule.fields)
        {
            RawValue part;
            if (!EvalRule(data, size, field.second, part))
            {
                return false;
            }
            values.emplace(field.first, part.isText ? part.text : FormatNumber(part));
        }

        std::wstring text;
        const std::wstring& templ = rule.formatTemplate;
        for (size_t i = 0; i < templ.size();)
        {
            if (templ[i] == L'{')
            {
                const size_t end = templ.find(L'}', i + 1);
                if (end != std::wstring::npos)
                {
                    const std::wstring key = templ.substr(i + 1, end - (i + 1));
                    const auto it = values.find(key);
                    if (it != values.end())
                    {
                        text += it->second;
                        i = end + 1;
                        continue;
                    }
                }
            }
            text.push_back(templ[i]);
            ++i;
        }

        out = {};
        out.isText = true;
        out.text = text;
        return true;
    }

    bool EvalRule(const uint8_t* data, size_t size, const Rule& rule, RawValue& out)
    {
        switch (rule.kind)
        {
        case RuleKind::Absolute:
        case RuleKind::Indirect:
        {
            uint64_t address = 0;
            return Locate(data, size, rule, address) && ReadScalar(data, size, address, rule.type, out);
        }
        case RuleKind::Flags:
            return EvalFlags(data, size, rule, out);
        case RuleKind::Format:
            return EvalFormat(data, size, rule, out);
        }
        return false;
    }
}

bool EvaluateColumn(const uint8_t* data, size_t size, const ColumnDef& column, EvaluatedValue& out, const StrRefSource* strrefs)
{
    out = {};
    if (data == nullptr && size != 0)
    {
        return false;
    }

    RawValue raw;
    if (!EvalRule(data, size, column.rule, raw))
    {
        return false;
    }

    switch (column.valueKind)
    {
    case ValueKind::String:
    case ValueKind::Resref:
        out.kind = EvalKind::String;
        out.text = raw.isText ? raw.text : FormatNumber(raw);
        return true;
    case ValueKind::Enum:
    {
        out.kind = EvalKind::String;
        const int64_t key = raw.isSigned ? raw.i : static_cast<int64_t>(raw.u);
        const auto it = column.enumLabels.find(key);
        out.text = it != column.enumLabels.end() ? it->second : FormatNumber(raw);
        return true;
    }
    case ValueKind::Int:
        out.kind = EvalKind::Int32;
        out.i = raw.isSigned ? raw.i : static_cast<int64_t>(raw.u);
        return true;
    case ValueKind::UInt:
        out.kind = EvalKind::UInt32;
        out.u = raw.u;
        return true;
    case ValueKind::StrRef:
    {
        out.kind = EvalKind::String;
        const uint32_t strref = static_cast<uint32_t>(raw.u);
        if (!raw.isText && strrefs != nullptr && ReadTlkString(strrefs->data, strrefs->size, strref, out.text))
        {
            return true;
        }
        wchar_t buf[32] = {};
        StringCchPrintfW(buf, ARRAYSIZE(buf), L"%d", static_cast<int>(static_cast<int32_t>(strref)));
        out.text = buf;
        return true;
    }
    }
    return false;
}
