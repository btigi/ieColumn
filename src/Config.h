#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

enum class ScalarType
{
    U8,
    I8,
    U16,
    I16,
    U32,
    I32,
    Resref
};

enum class ValueKind
{
    UInt,
    Int,
    String,
    Enum,
    Resref,
    StrRef
};

enum class RuleKind
{
    Absolute,
    Indirect,
    Format,
    Flags
};

struct Rule
{
    RuleKind kind = RuleKind::Absolute;
    bool indirect = false;
    uint64_t offset = 0;
    uint64_t baseOffset = 0;
    uint64_t index = 0;
    uint64_t stride = 0;
    uint64_t fieldOffset = 0;
    ScalarType type = ScalarType::U32;
    std::wstring formatTemplate;
    std::vector<std::pair<std::wstring, Rule>> fields;
    std::map<uint32_t, std::wstring> bitLabels;
};

struct ColumnDef
{
    std::wstring id;
    std::wstring label;
    std::wstring description;
    uint32_t propId = 0;
    ValueKind valueKind = ValueKind::UInt;
    ScalarType defaultReadType = ScalarType::U32;
    bool hasDefaultReadType = false;
    bool enabled = true;
    Rule rule;
    std::map<int64_t, std::wstring> enumLabels;
};

struct FileSignature
{
    uint64_t offset = 0;
    std::vector<uint8_t> bytes;
};

struct TypeHandler
{
    std::wstring id;
    std::vector<std::wstring> extensions;
    FileSignature signature;
    GUID formatId = {};
    std::vector<ColumnDef> columns;
};

struct HandlerConfig
{
    std::vector<TypeHandler> handlers;
};

constexpr wchar_t kConfigFileName[] = L"ieColumn.json";
constexpr wchar_t kPropDescFileName[] = L"ieColumn.propdesc";
constexpr wchar_t kConfigRegKey[] = L"SOFTWARE\\InfinityEngine\\ieColumn";
constexpr wchar_t kConfigRegValue[] = L"ConfigPath";
constexpr wchar_t kRegisteredExtensionsValue[] = L"RegisteredExtensions";

HRESULT LoadHandlerConfigFromUtf8(const char* utf8, size_t length, HandlerConfig& out, std::wstring& error);
HRESULT LoadHandlerConfigFile(const std::wstring& path, HandlerConfig& out, std::wstring& error);
HRESULT LoadCachedHandlerConfig(HINSTANCE module, HandlerConfig& out, std::wstring& error);
std::wstring ResolveConfigPath(HINSTANCE module);
std::wstring ExtensionFromPath(const std::wstring& path);
bool MatchesSignature(const uint8_t* data, size_t size, const FileSignature& signature);
const TypeHandler* FindTypeHandler(const HandlerConfig& config, const std::wstring& extension, const uint8_t* data, size_t size);
std::vector<std::wstring> CollectExtensions(const HandlerConfig& config);
