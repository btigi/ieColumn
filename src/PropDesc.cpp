#include "PropDesc.h"

#include <strsafe.h>

namespace
{
    std::wstring XmlEscape(const std::wstring& text)
    {
        std::wstring out;
        out.reserve(text.size());
        for (wchar_t ch : text)
        {
            switch (ch)
            {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            default: out.push_back(ch); break;
            }
        }
        return out;
    }

    const wchar_t* SchemaType(ValueKind kind)
    {
        switch (kind)
        {
        case ValueKind::Int:
            return L"Int32";
        case ValueKind::UInt:
            return L"UInt32";
        case ValueKind::String:
        case ValueKind::Enum:
        case ValueKind::Resref:
        case ValueKind::StrRef:
            return L"String";
        }
        return L"String";
    }

    const wchar_t* DisplayType(ValueKind kind)
    {
        switch (kind)
        {
        case ValueKind::Int:
        case ValueKind::UInt:
            return L"Number";
        case ValueKind::String:
        case ValueKind::Enum:
        case ValueKind::Resref:
        case ValueKind::StrRef:
            return L"String";
        }
        return L"String";
    }
}

std::wstring PropertyCanonicalName(const std::wstring& handlerId, const ColumnDef& column)
{
    return L"InfinityEngine." + handlerId + L"." + column.id;
}

std::wstring BuildPropList(const TypeHandler& handler)
{
    std::wstring list = L"prop:System.PropGroup.Description";
    for (const ColumnDef& column : handler.columns)
    {
        if (!column.enabled)
        {
            continue;
        }
        list += L";";
        list += PropertyCanonicalName(handler.id, column);
    }
    return list;
}

std::wstring BuildPropertySchemaXml(const HandlerConfig& config)
{
    std::wstring xml;
    xml += L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    xml += L"<schema xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"\r\n";
    xml += L"        xmlns=\"http://schemas.microsoft.com/windows/2006/propertydescription\"\r\n";
    xml += L"        schemaVersion=\"1.0\">\r\n";
    xml += L"  <propertyDescriptionList publisher=\"InfinityEngine\" product=\"ieColumn\">\r\n";

    for (const TypeHandler& handler : config.handlers)
    {
        wchar_t formatId[64] = {};
        StringFromGUID2(handler.formatId, formatId, ARRAYSIZE(formatId));
        for (const ColumnDef& column : handler.columns)
        {
            if (!column.enabled)
            {
                continue;
            }
            const std::wstring description = column.description.empty() ? column.label : column.description;
            wchar_t propId[32] = {};
            StringCchPrintfW(propId, ARRAYSIZE(propId), L"%u", column.propId);

            xml += L"    <propertyDescription name=\"";
            xml += XmlEscape(PropertyCanonicalName(handler.id, column));
            xml += L"\" formatID=\"";
            xml += formatId;
            xml += L"\" propID=\"";
            xml += propId;
            xml += L"\">\r\n";
            xml += L"      <description>";
            xml += XmlEscape(description);
            xml += L"</description>\r\n";
            xml += L"      <searchInfo inInvertedIndex=\"false\" isColumn=\"true\"/>\r\n";
            xml += L"      <typeInfo type=\"";
            xml += SchemaType(column.valueKind);
            xml += L"\" multipleValues=\"false\" isViewable=\"true\" isQueryable=\"true\" isInnate=\"true\"/>\r\n";
            xml += L"      <labelInfo label=\"";
            xml += XmlEscape(column.label);
            xml += L"\"/>\r\n";
            xml += L"      <displayInfo displayType=\"";
            xml += DisplayType(column.valueKind);
            xml += L"\"/>\r\n";
            xml += L"    </propertyDescription>\r\n";
        }
    }

    xml += L"  </propertyDescriptionList>\r\n";
    xml += L"</schema>\r\n";
    return xml;
}

HRESULT WritePropertySchemaFile(const HandlerConfig& config, const std::wstring& path, std::wstring& error)
{
    error.clear();
    const std::wstring xml = BuildPropertySchemaXml(config);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, xml.data(), static_cast<int>(xml.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
    {
        error = L"Could not encode the property schema";
        return E_FAIL;
    }

    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, xml.data(), static_cast<int>(xml.size()), utf8.data(), bytes, nullptr, nullptr);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr)
    {
        error = L"Cannot write property schema: " + path;
        return E_FAIL;
    }

    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    const bool wrote = fwrite(bom, 1, sizeof(bom), file) == sizeof(bom)
        && fwrite(utf8.data(), 1, utf8.size(), file) == utf8.size();
    fclose(file);
    if (!wrote)
    {
        error = L"Cannot write property schema: " + path;
        return E_FAIL;
    }
    return S_OK;
}
