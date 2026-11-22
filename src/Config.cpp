#include "Config.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <stdexcept>

namespace
{
    constexpr size_t kMaxConfigBytes = 1024 * 1024;
    constexpr size_t kMaxColumns = 256;
    constexpr size_t kMaxHandlers = 64;
    constexpr size_t kMaxSignatureBytes = 64;

    std::wstring Utf8ToWide(const char* text, int length)
    {
        if (text == nullptr || length <= 0)
        {
            return {};
        }
        const int count = MultiByteToWideChar(CP_UTF8, 0, text, length, nullptr, 0);
        if (count <= 0)
        {
            return {};
        }
        std::wstring wide(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text, length, wide.data(), count);
        return wide;
    }

    std::wstring Utf8ToWide(const std::string& text)
    {
        return Utf8ToWide(text.data(), static_cast<int>(text.size()));
    }

    std::string Narrow(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0)
        {
            return {};
        }
        std::string utf8(static_cast<size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), count, nullptr, nullptr);
        return utf8;
    }

    [[noreturn]] void Fail(const std::wstring& message)
    {
        throw std::runtime_error(Narrow(message));
    }

    bool IsIdentifier(const std::wstring& id)
    {
        if (id.empty())
        {
            return false;
        }
        const wchar_t first = id[0];
        if (!((first >= L'A' && first <= L'Z') || (first >= L'a' && first <= L'z') || first == L'_'))
        {
            return false;
        }
        for (wchar_t ch : id)
        {
            const bool ok = (ch >= L'A' && ch <= L'Z')
                || (ch >= L'a' && ch <= L'z')
                || (ch >= L'0' && ch <= L'9')
                || ch == L'_';
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    bool ParseScalarType(const std::string& text, ScalarType& type)
    {
        if (text == "u8") type = ScalarType::U8;
        else if (text == "i8") type = ScalarType::I8;
        else if (text == "u16") type = ScalarType::U16;
        else if (text == "i16") type = ScalarType::I16;
        else if (text == "u32") type = ScalarType::U32;
        else if (text == "i32") type = ScalarType::I32;
        else if (text == "resref") type = ScalarType::Resref;
        else return false;
        return true;
    }

    uint64_t ParseOffset(const nlohmann::json& node, const std::wstring& where)
    {
        if (node.is_number_unsigned())
        {
            return node.get<uint64_t>();
        }
        if (node.is_number_integer())
        {
            const int64_t value = node.get<int64_t>();
            if (value < 0)
            {
                Fail(where + L" offset is negative");
            }
            return static_cast<uint64_t>(value);
        }
        if (node.is_string())
        {
            const std::string text = node.get<std::string>();
            if (text.empty())
            {
                Fail(where + L" offset is empty");
            }
            try
            {
                size_t used = 0;
                const unsigned long long value = std::stoull(text, &used, 0);
                if (used != text.size())
                {
                    Fail(where + L" offset is not a number");
                }
                return static_cast<uint64_t>(value);
            }
            catch (const std::exception&)
            {
                Fail(where + L" offset is not a number");
            }
        }
        Fail(where + L" offset must be a number or hex string");
    }

    const nlohmann::json& Require(const nlohmann::json& obj, const char* key, const std::wstring& where)
    {
        if (!obj.is_object() || !obj.contains(key))
        {
            Fail(where + L" is missing \"" + Utf8ToWide(key) + L"\"");
        }
        return obj.at(key);
    }

    void ParseBitLabels(const nlohmann::json& node, const std::wstring& where, std::map<uint32_t, std::wstring>& bits)
    {
        if (!node.is_object())
        {
            Fail(where + L" bits must be an object");
        }
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            if (!it.value().is_string())
            {
                Fail(where + L" bit label must be a string");
            }
            unsigned long bit = 0;
            try
            {
                size_t used = 0;
                bit = std::stoul(it.key(), &used, 0);
                if (used != it.key().size())
                {
                    Fail(where + L" bit index is not a number");
                }
            }
            catch (const std::exception&)
            {
                Fail(where + L" bit index is not a number");
            }
            bits[static_cast<uint32_t>(bit)] = Utf8ToWide(it.value().get<std::string>());
        }
    }

    Rule ParseRule(const nlohmann::json& node, const ColumnDef& column, const std::wstring& where, int depth)
    {
        if (depth > 8)
        {
            Fail(where + L" rule is nested too deeply");
        }
        if (!node.is_object())
        {
            Fail(where + L" rule must be an object");
        }

        const auto& kindNode = Require(node, "kind", where);
        if (!kindNode.is_string())
        {
            Fail(where + L" kind must be a string");
        }

        Rule rule;
        const std::string kind = kindNode.get<std::string>();
        const bool isIndirect = kind == "indirect";
        const bool isAbsolute = kind == "absolute";
        const bool isFormat = kind == "format";
        const bool isFlags = kind == "flags";
        if (!isIndirect && !isAbsolute && !isFormat && !isFlags)
        {
            Fail(where + L" kind must be absolute, indirect, format, or flags");
        }

        auto readType = [&](ScalarType& type) {
            if (node.contains("type"))
            {
                if (!node.at("type").is_string() || !ParseScalarType(node.at("type").get<std::string>(), type))
                {
                    Fail(where + L" type must be u8, i8, u16, i16, u32, i32, or resref");
                }
                return;
            }
            if (!column.hasDefaultReadType)
            {
                Fail(where + L" needs a type");
            }
            type = column.defaultReadType;
        };

        if (isAbsolute || isIndirect)
        {
            rule.kind = isIndirect ? RuleKind::Indirect : RuleKind::Absolute;
            rule.indirect = isIndirect;
            readType(rule.type);
            if (isIndirect)
            {
                rule.baseOffset = ParseOffset(Require(node, "baseOffset", where), where);
                rule.fieldOffset = ParseOffset(Require(node, "fieldOffset", where), where);
                rule.stride = ParseOffset(Require(node, "stride", where), where);
                if (node.contains("index"))
                {
                    rule.index = ParseOffset(node.at("index"), where);
                }
            }
            else
            {
                rule.offset = ParseOffset(Require(node, "offset", where), where);
            }
            return rule;
        }

        if (isFormat)
        {
            rule.kind = RuleKind::Format;
            const auto& templ = Require(node, "template", where);
            if (!templ.is_string())
            {
                Fail(where + L" template must be a string");
            }
            rule.formatTemplate = Utf8ToWide(templ.get<std::string>());
            const auto& fields = Require(node, "fields", where);
            if (!fields.is_object() || fields.empty())
            {
                Fail(where + L" fields must be a non-empty object");
            }
            for (auto it = fields.begin(); it != fields.end(); ++it)
            {
                const std::wstring name = Utf8ToWide(it.key());
                if (name.empty())
                {
                    Fail(where + L" has an empty field name");
                }
                rule.fields.emplace_back(name, ParseRule(it.value(), column, where + L" field " + name, depth + 1));
            }
            return rule;
        }

        rule.kind = RuleKind::Flags;
        readType(rule.type);
        if (rule.type == ScalarType::Resref)
        {
            Fail(where + L" flags cannot use resref");
        }
        if (node.contains("baseOffset"))
        {
            rule.indirect = true;
            rule.baseOffset = ParseOffset(node.at("baseOffset"), where);
            rule.fieldOffset = ParseOffset(Require(node, "fieldOffset", where), where);
            rule.stride = ParseOffset(Require(node, "stride", where), where);
            if (node.contains("index"))
            {
                rule.index = ParseOffset(node.at("index"), where);
            }
        }
        else
        {
            rule.offset = ParseOffset(Require(node, "offset", where), where);
        }
        ParseBitLabels(Require(node, "bits", where), where, rule.bitLabels);
        if (rule.bitLabels.empty())
        {
            Fail(where + L" bits must name at least one bit");
        }
        return rule;
    }

    void ParseEnumLabels(const nlohmann::json& node, const std::wstring& where, std::map<int64_t, std::wstring>& labels)
    {
        if (!node.is_object())
        {
            Fail(where + L" enum must be an object");
        }
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            if (!it.value().is_string())
            {
                Fail(where + L" enum label must be a string");
            }
            long long key = 0;
            try
            {
                size_t used = 0;
                key = std::stoll(it.key(), &used, 0);
                if (used != it.key().size())
                {
                    Fail(where + L" enum key is not an integer");
                }
            }
            catch (const std::exception&)
            {
                Fail(where + L" enum key is not an integer");
            }
            labels[static_cast<int64_t>(key)] = Utf8ToWide(it.value().get<std::string>());
        }
    }

    void ToLowerAscii(std::wstring& text)
    {
        for (wchar_t& ch : text)
        {
            if (ch >= L'A' && ch <= L'Z')
            {
                ch = static_cast<wchar_t>(ch - L'A' + L'a');
            }
        }
    }

    std::wstring ParseExtension(const nlohmann::json& node, const std::wstring& where)
    {
        if (!node.is_string())
        {
            Fail(where + L" extension must be a string");
        }
        std::wstring ext = Utf8ToWide(node.get<std::string>());
        if (ext.size() < 2 || ext[0] != L'.')
        {
            Fail(where + L" extension must start with a dot: " + ext);
        }
        for (size_t i = 1; i < ext.size(); ++i)
        {
            const wchar_t ch = ext[i];
            const bool ok = (ch >= L'A' && ch <= L'Z')
                || (ch >= L'a' && ch <= L'z')
                || (ch >= L'0' && ch <= L'9')
                || ch == L'_';
            if (!ok)
            {
                Fail(where + L" extension has an invalid character: " + ext);
            }
        }
        ToLowerAscii(ext);
        return ext;
    }

    int HexNibble(char ch)
    {
        if (ch >= '0' && ch <= '9')
        {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f')
        {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F')
        {
            return ch - 'A' + 10;
        }
        return -1;
    }

    FileSignature ParseSignature(const nlohmann::json& node, const std::wstring& where)
    {
        if (!node.is_object())
        {
            Fail(where + L" must be an object");
        }
        FileSignature signature;
        signature.offset = ParseOffset(Require(node, "offset", where), where);
        const auto& bytesNode = Require(node, "bytes", where);
        if (!bytesNode.is_string())
        {
            Fail(where + L" bytes must be a hex string");
        }
        const std::string hex = bytesNode.get<std::string>();
        if (hex.empty() || (hex.size() % 2) != 0)
        {
            Fail(where + L" bytes must be a non-empty even-length hex string");
        }
        if (hex.size() / 2 > kMaxSignatureBytes)
        {
            Fail(where + L" signature is longer than 64 bytes");
        }
        signature.bytes.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2)
        {
            const int hi = HexNibble(hex[i]);
            const int lo = HexNibble(hex[i + 1]);
            if (hi < 0 || lo < 0)
            {
                Fail(where + L" bytes must be hexadecimal");
            }
            signature.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        return signature;
    }

    std::vector<ColumnDef> ParseColumns(const nlohmann::json& columns, const std::wstring& handlerWhere)
    {
        if (!columns.is_array())
        {
            Fail(handlerWhere + L" columns must be an array");
        }
        if (columns.size() > kMaxColumns)
        {
            Fail(handlerWhere + L" has too many columns");
        }

        std::vector<ColumnDef> parsed;
        std::map<uint32_t, std::wstring> seenIds;
        std::map<std::wstring, uint32_t> seenNames;
        for (const auto& node : columns)
        {
            ColumnDef column;
            const std::wstring where = L"column";
            const auto& idNode = Require(node, "id", where);
            const auto& labelNode = Require(node, "label", where);
            const auto& propNode = Require(node, "propId", where);
            const auto& typeNode = Require(node, "valueType", where);
            if (!idNode.is_string() || !labelNode.is_string() || !typeNode.is_string() || !propNode.is_number_integer())
            {
                Fail(L"column id, label, valueType, and propId are invalid");
            }

            column.id = Utf8ToWide(idNode.get<std::string>());
            column.label = Utf8ToWide(labelNode.get<std::string>());
            if (!IsIdentifier(column.id))
            {
                Fail(L"column id must be an identifier: " + column.id);
            }
            if (seenNames.count(column.id) != 0)
            {
                Fail(L"duplicate column id: " + column.id);
            }
            seenNames.emplace(column.id, column.propId);
            if (column.label.empty())
            {
                Fail(L"column " + column.id + L" label is empty");
            }
            if (node.contains("description"))
            {
                if (!node.at("description").is_string())
                {
                    Fail(L"column " + column.id + L" description must be a string");
                }
                column.description = Utf8ToWide(node.at("description").get<std::string>());
            }
            if (node.contains("enabled"))
            {
                if (!node.at("enabled").is_boolean())
                {
                    Fail(L"column " + column.id + L" enabled must be a boolean");
                }
                column.enabled = node.at("enabled").get<bool>();
            }

            const int64_t propId = propNode.get<int64_t>();
            if (propId < 2 || propId > 0x7fffffff)
            {
                Fail(L"column " + column.id + L" propId must be between 2 and 2147483647");
            }
            column.propId = static_cast<uint32_t>(propId);
            if (seenIds.count(column.propId) != 0)
            {
                Fail(L"duplicate propId on columns " + seenIds[column.propId] + L" and " + column.id);
            }
            seenIds.emplace(column.propId, column.id);

            const std::string valueType = typeNode.get<std::string>();
            if (valueType == "string")
            {
                column.valueKind = ValueKind::String;
            }
            else if (valueType == "strref")
            {
                column.valueKind = ValueKind::StrRef;
                column.defaultReadType = ScalarType::U32;
                column.hasDefaultReadType = true;
            }
            else if (valueType == "enum")
            {
                column.valueKind = ValueKind::Enum;
            }
            else if (valueType == "resref")
            {
                column.valueKind = ValueKind::Resref;
                column.defaultReadType = ScalarType::Resref;
                column.hasDefaultReadType = true;
            }
            else if (ParseScalarType(valueType, column.defaultReadType))
            {
                column.hasDefaultReadType = true;
                switch (column.defaultReadType)
                {
                case ScalarType::I8:
                case ScalarType::I16:
                case ScalarType::I32:
                    column.valueKind = ValueKind::Int;
                    break;
                case ScalarType::Resref:
                    column.valueKind = ValueKind::Resref;
                    break;
                default:
                    column.valueKind = ValueKind::UInt;
                    break;
                }
            }
            else
            {
                Fail(L"column " + column.id + L" has an unknown valueType");
            }

            column.rule = ParseRule(Require(node, "rule", L"column " + column.id), column, L"column " + column.id, 0);
            if (node.contains("enum"))
            {
                ParseEnumLabels(node.at("enum"), L"column " + column.id, column.enumLabels);
            }

            const bool textRule = column.rule.kind == RuleKind::Format || column.rule.kind == RuleKind::Flags;
            if (textRule && column.valueKind != ValueKind::String)
            {
                Fail(L"column " + column.id + L" format/flags rules require valueType string");
            }
            if (column.valueKind == ValueKind::Enum && column.enumLabels.empty())
            {
                Fail(L"column " + column.id + L" enum needs an enum table");
            }
            if (column.valueKind == ValueKind::Enum && textRule)
            {
                Fail(L"column " + column.id + L" enum cannot use a text rule");
            }
            if ((column.valueKind == ValueKind::Int || column.valueKind == ValueKind::UInt || column.valueKind == ValueKind::StrRef) && textRule)
            {
                Fail(L"column " + column.id + L" numeric valueType cannot use a text rule");
            }

            parsed.push_back(std::move(column));
        }

        return parsed;
    }

    TypeHandler ParseTypeHandler(const nlohmann::json& node, size_t index)
    {
        const std::wstring where = L"handler " + std::to_wstring(index);
        if (!node.is_object())
        {
            Fail(where + L" must be an object");
        }

        TypeHandler handler;
        const auto& idNode = Require(node, "id", where);
        if (!idNode.is_string())
        {
            Fail(where + L" id must be a string");
        }
        handler.id = Utf8ToWide(idNode.get<std::string>());
        if (!IsIdentifier(handler.id))
        {
            Fail(where + L" id must be an identifier: " + handler.id);
        }

        const auto& extensions = Require(node, "extensions", where);
        if (!extensions.is_array() || extensions.empty())
        {
            Fail(where + L" extensions must be a non-empty array");
        }
        for (const auto& extension : extensions)
        {
            const std::wstring ext = ParseExtension(extension, where);
            for (const std::wstring& existing : handler.extensions)
            {
                if (existing == ext)
                {
                    Fail(where + L" lists extension " + ext + L" more than once");
                }
            }
            handler.extensions.push_back(ext);
        }

        const auto& formatId = Require(node, "formatId", where);
        if (!formatId.is_string())
        {
            Fail(where + L" formatId must be a GUID string");
        }
        const std::wstring formatText = Utf8ToWide(formatId.get<std::string>());
        if (FAILED(CLSIDFromString(formatText.c_str(), &handler.formatId)) || handler.formatId == GUID_NULL)
        {
            Fail(where + L" formatId is not a GUID");
        }

        if (node.contains("signature"))
        {
            handler.signature = ParseSignature(node.at("signature"), where + L" signature");
        }

        handler.columns = ParseColumns(Require(node, "columns", where), where);
        return handler;
    }

    HandlerConfig ParseConfig(const nlohmann::json& root)
    {
        if (!root.is_object())
        {
            Fail(L"Config root must be an object");
        }

        const auto& handlers = Require(root, "handlers", L"config");
        if (!handlers.is_array() || handlers.empty())
        {
            Fail(L"handlers must be a non-empty array");
        }
        if (handlers.size() > kMaxHandlers)
        {
            Fail(L"Too many handlers");
        }

        HandlerConfig config;
        std::map<std::wstring, std::wstring> seenIds;
        std::map<std::wstring, std::wstring> seenExtensions;
        for (size_t i = 0; i < handlers.size(); ++i)
        {
            TypeHandler handler = ParseTypeHandler(handlers[i], i);
            if (seenIds.count(handler.id) != 0)
            {
                Fail(L"duplicate handler id: " + handler.id);
            }
            seenIds.emplace(handler.id, handler.id);

            for (const std::wstring& ext : handler.extensions)
            {
                const auto existing = seenExtensions.find(ext);
                if (existing != seenExtensions.end())
                {
                    Fail(L"extension " + ext + L" is used by handlers " + existing->second + L" and " + handler.id);
                }
                seenExtensions.emplace(ext, handler.id);
            }

            for (const TypeHandler& previous : config.handlers)
            {
                if (IsEqualGUID(previous.formatId, handler.formatId))
                {
                    Fail(L"duplicate formatId on handlers " + previous.id + L" and " + handler.id);
                }
            }

            config.handlers.push_back(std::move(handler));
        }

        return config;
    }

    std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* name)
    {
        DWORD bytes = 0;
        LSTATUS status = RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
        if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        {
            return {};
        }
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        status = RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes);
        if (status != ERROR_SUCCESS)
        {
            return {};
        }
        value.resize(wcsnlen(value.c_str(), value.size()));
        return value;
    }

    std::wstring ModuleDirectory(HINSTANCE module)
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return {};
        }
        path.resize(slash);
        return path;
    }
}

std::wstring ResolveConfigPath(HINSTANCE module)
{
    const std::wstring overridePath = ReadRegString(HKEY_LOCAL_MACHINE, kConfigRegKey, kConfigRegValue);
    if (!overridePath.empty())
    {
        return overridePath;
    }
    const std::wstring dir = ModuleDirectory(module);
    if (dir.empty())
    {
        return kConfigFileName;
    }
    return dir + L"\\" + kConfigFileName;
}

HRESULT LoadHandlerConfigFromUtf8(const char* utf8, size_t length, HandlerConfig& out, std::wstring& error)
{
    out = {};
    error.clear();
    if (utf8 == nullptr && length != 0)
    {
        error = L"Config text is missing";
        return E_INVALIDARG;
    }
    if (length >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF
        && static_cast<unsigned char>(utf8[1]) == 0xBB
        && static_cast<unsigned char>(utf8[2]) == 0xBF)
    {
        utf8 += 3;
        length -= 3;
    }
    if (length > kMaxConfigBytes)
    {
        error = L"Config file is larger than 1 MB";
        return E_FAIL;
    }

    try
    {
        const nlohmann::json root = nlohmann::json::parse(utf8, utf8 + length, nullptr, true, true);
        out = ParseConfig(root);
        return S_OK;
    }
    catch (const std::exception& ex)
    {
        error = Utf8ToWide(ex.what());
        if (error.empty())
        {
            error = L"Config parse failed";
        }
        return E_FAIL;
    }
}

HRESULT LoadHandlerConfigFile(const std::wstring& path, HandlerConfig& out, std::wstring& error)
{
    out = {};
    error.clear();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        error = L"Cannot open config: " + path;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > kMaxConfigBytes)
    {
        CloseHandle(file);
        error = L"Config file is missing or larger than 1 MB: " + path;
        return E_FAIL;
    }

    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = text.empty() || ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != text.size())
    {
        error = L"Cannot read config: " + path;
        return E_FAIL;
    }
    return LoadHandlerConfigFromUtf8(text.data(), text.size(), out, error);
}

HRESULT LoadCachedHandlerConfig(HINSTANCE module, HandlerConfig& out, std::wstring& error)
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static HandlerConfig cached;
    static std::wstring cachedPath;
    static FILETIME cachedWrite = {};
    static bool loaded = false;

    struct SharedLock
    {
        SRWLOCK* lock;
        explicit SharedLock(SRWLOCK* value) : lock(value) { AcquireSRWLockShared(lock); }
        ~SharedLock() { ReleaseSRWLockShared(lock); }
    };
    struct ExclusiveLock
    {
        SRWLOCK* lock;
        explicit ExclusiveLock(SRWLOCK* value) : lock(value) { AcquireSRWLockExclusive(lock); }
        ~ExclusiveLock() { ReleaseSRWLockExclusive(lock); }
    };

    const std::wstring path = ResolveConfigPath(module);
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
    {
        error = L"Cannot open config: " + path;
        return HRESULT_FROM_WIN32(GetLastError());
    }

    {
        SharedLock shared(&lock);
        if (loaded && cachedPath == path && CompareFileTime(&cachedWrite, &info.ftLastWriteTime) == 0)
        {
            out = cached;
            return S_OK;
        }
    }

    HandlerConfig fresh;
    const HRESULT hr = LoadHandlerConfigFile(path, fresh, error);
    if (FAILED(hr))
    {
        return hr;
    }

    ExclusiveLock exclusive(&lock);
    cached = fresh;
    cachedPath = path;
    cachedWrite = info.ftLastWriteTime;
    loaded = true;
    out = cached;
    return S_OK;
}

std::wstring ExtensionFromPath(const std::wstring& path)
{
    if (path.empty())
    {
        return {};
    }
    const size_t slash = path.find_last_of(L"\\/");
    const size_t nameStart = slash == std::wstring::npos ? 0 : slash + 1;
    if (nameStart >= path.size())
    {
        return {};
    }
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || dot <= nameStart || dot + 1 >= path.size())
    {
        return {};
    }
    std::wstring ext = path.substr(dot);
    for (wchar_t& ch : ext)
    {
        if (ch >= L'A' && ch <= L'Z')
        {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return ext;
}

bool MatchesSignature(const uint8_t* data, size_t size, const FileSignature& signature)
{
    if (signature.bytes.empty())
    {
        return true;
    }
    if (data == nullptr || signature.offset > size || signature.bytes.size() > size - static_cast<size_t>(signature.offset))
    {
        return false;
    }
    return memcmp(data + static_cast<size_t>(signature.offset), signature.bytes.data(), signature.bytes.size()) == 0;
}

const TypeHandler* FindTypeHandler(const HandlerConfig& config, const std::wstring& extension, const uint8_t* data, size_t size)
{
    std::wstring ext = extension;
    for (wchar_t& ch : ext)
    {
        if (ch >= L'A' && ch <= L'Z')
        {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }

    if (!ext.empty())
    {
        for (const TypeHandler& handler : config.handlers)
        {
            for (const std::wstring& owned : handler.extensions)
            {
                if (owned != ext)
                {
                    continue;
                }
                if (handler.signature.bytes.empty() || MatchesSignature(data, size, handler.signature))
                {
                    return &handler;
                }
                return nullptr;
            }
        }
    }

    for (const TypeHandler& handler : config.handlers)
    {
        if (!handler.signature.bytes.empty() && MatchesSignature(data, size, handler.signature))
        {
            return &handler;
        }
    }
    return nullptr;
}

std::vector<std::wstring> CollectExtensions(const HandlerConfig& config)
{
    std::vector<std::wstring> extensions;
    for (const TypeHandler& handler : config.handlers)
    {
        extensions.insert(extensions.end(), handler.extensions.begin(), handler.extensions.end());
    }
    return extensions;
}
