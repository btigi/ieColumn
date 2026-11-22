#include "RegisterExtension.h"

#include "Config.h"
#include "Dll.h"
#include "ieColumn.h"
#include "PropDesc.h"

#include <propsys.h>
#include <shlobj.h>

namespace
{
    std::wstring ModuleDirectory()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(g_hInst, path.data(), static_cast<DWORD>(path.size()));
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

    std::wstring ModulePath()
    {
        std::wstring path(32768, L'\0');
        const DWORD length = GetModuleFileNameW(g_hInst, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
        {
            return {};
        }
        path.resize(length);
        return path;
    }

    HRESULT SetString(HKEY root, const std::wstring& subkey, const wchar_t* name, const std::wstring& value)
    {
        HKEY key = nullptr;
        const LSTATUS created = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (created != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(created);
        }
        const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        const LSTATUS status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
        RegCloseKey(key);
        return HRESULT_FROM_WIN32(status);
    }

    HRESULT SetDword(HKEY root, const std::wstring& subkey, const wchar_t* name, DWORD value)
    {
        HKEY key = nullptr;
        const LSTATUS created = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (created != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(created);
        }
        const LSTATUS status = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
        return HRESULT_FROM_WIN32(status);
    }

    void DeleteValue(HKEY root, const wchar_t* subkey, const wchar_t* name)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, subkey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
        {
            RegDeleteValueW(key, name);
            RegCloseKey(key);
        }
    }

    std::vector<std::wstring> ReadMultiSz(HKEY root, const wchar_t* subkey, const wchar_t* name)
    {
        DWORD bytes = 0;
        if (RegGetValueW(root, subkey, name, RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        {
            return {};
        }
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, L'\0');
        if (RegGetValueW(root, subkey, name, RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &bytes) != ERROR_SUCCESS)
        {
            return {};
        }
        std::vector<std::wstring> values;
        const wchar_t* cursor = buffer.data();
        const wchar_t* end = buffer.data() + (bytes / sizeof(wchar_t));
        while (cursor < end && *cursor != L'\0')
        {
            values.emplace_back(cursor);
            cursor += values.back().size() + 1;
        }
        return values;
    }

    HRESULT WriteMultiSz(HKEY root, const wchar_t* subkey, const wchar_t* name, const std::vector<std::wstring>& values)
    {
        std::vector<wchar_t> buffer;
        for (const std::wstring& value : values)
        {
            buffer.insert(buffer.end(), value.begin(), value.end());
            buffer.push_back(L'\0');
        }
        buffer.push_back(L'\0');

        HKEY key = nullptr;
        const LSTATUS created = RegCreateKeyExW(root, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (created != ERROR_SUCCESS)
        {
            return HRESULT_FROM_WIN32(created);
        }
        const DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        const LSTATUS status = RegSetValueExW(key, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(buffer.data()), bytes);
        RegCloseKey(key);
        return HRESULT_FROM_WIN32(status);
    }

    void RemoveOwnedExtension(const std::wstring& extension, const wchar_t* clsid)
    {
        const std::wstring handlerKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\" + extension;
        wchar_t registered[64] = {};
        DWORD registeredBytes = sizeof(registered);
        const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, handlerKey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, registered, &registeredBytes);
        const bool missing = status == ERROR_FILE_NOT_FOUND;
        const bool owned = status == ERROR_SUCCESS && CompareStringOrdinal(registered, -1, clsid, -1, TRUE) == CSTR_EQUAL;
        if (owned)
        {
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, handlerKey.c_str());
        }
        if (owned || missing)
        {
            const std::wstring assoc = L"SystemFileAssociations\\" + extension;
            DeleteValue(HKEY_CLASSES_ROOT, assoc.c_str(), L"FullDetails");
            DeleteValue(HKEY_CLASSES_ROOT, assoc.c_str(), L"PreviewDetails");
            DeleteValue(HKEY_CLASSES_ROOT, assoc.c_str(), L"InfoTip");
        }
    }
}

HRESULT RegisterHandler()
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = coInit == S_OK || coInit == S_FALSE;

    const std::wstring dir = ModuleDirectory();
    const std::wstring module = ModulePath();
    HRESULT hr = (dir.empty() || module.empty()) ? E_FAIL : S_OK;
    std::wstring error;

    HandlerConfig config;
    if (SUCCEEDED(hr))
    {
        hr = LoadHandlerConfigFile(ResolveConfigPath(g_hInst), config, error);
    }

    const std::wstring schemaPath = dir + L"\\" + kPropDescFileName;
    if (SUCCEEDED(hr))
    {
        const DWORD attrs = GetFileAttributesW(schemaPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES)
        {
            PSUnregisterPropertySchema(schemaPath.c_str());
        }
        hr = WritePropertySchemaFile(config, schemaPath, error);
    }

    wchar_t clsid[64] = {};
    if (SUCCEEDED(hr))
    {
        StringFromGUID2(__uuidof(CieColumn), clsid, ARRAYSIZE(clsid));
        const std::vector<std::wstring> previous = ReadMultiSz(HKEY_LOCAL_MACHINE, kConfigRegKey, kRegisteredExtensionsValue);
        for (const std::wstring& extension : previous)
        {
            RemoveOwnedExtension(extension, clsid);
        }

        const std::wstring clsidKey = std::wstring(L"CLSID\\") + clsid;
        hr = SetString(HKEY_CLASSES_ROOT, clsidKey, nullptr, L"ieColumn Property Handler");
        if (SUCCEEDED(hr))
        {
            hr = SetDword(HKEY_CLASSES_ROOT, clsidKey, L"DisableProcessIsolation", 1);
        }
        if (SUCCEEDED(hr))
        {
            hr = SetString(HKEY_CLASSES_ROOT, clsidKey + L"\\InProcServer32", nullptr, module);
        }
        if (SUCCEEDED(hr))
        {
            hr = SetString(HKEY_CLASSES_ROOT, clsidKey + L"\\InProcServer32", L"ThreadingModel", L"Both");
        }
    }

    const std::vector<std::wstring> extensions = CollectExtensions(config);
    if (SUCCEEDED(hr))
    {
        for (const TypeHandler& handler : config.handlers)
        {
            const std::wstring props = BuildPropList(handler);
            for (const std::wstring& extension : handler.extensions)
            {
                hr = SetString(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\" + extension,
                    nullptr,
                    clsid);
                if (FAILED(hr))
                {
                    break;
                }
                const std::wstring assoc = L"SystemFileAssociations\\" + extension;
                hr = SetString(HKEY_CLASSES_ROOT, assoc, L"FullDetails", props);
                if (SUCCEEDED(hr))
                {
                    hr = SetString(HKEY_CLASSES_ROOT, assoc, L"PreviewDetails", props);
                }
                if (SUCCEEDED(hr))
                {
                    hr = SetString(HKEY_CLASSES_ROOT, assoc, L"InfoTip", props);
                }
                if (FAILED(hr))
                {
                    break;
                }
            }
            if (FAILED(hr))
            {
                break;
            }
        }
    }

    if (SUCCEEDED(hr))
    {
        hr = WriteMultiSz(HKEY_LOCAL_MACHINE, kConfigRegKey, kRegisteredExtensionsValue, extensions);
    }

    if (SUCCEEDED(hr))
    {
        hr = PSRegisterPropertySchema(schemaPath.c_str());
        if (FAILED(hr) && error.empty())
        {
            error = L"PSRegisterPropertySchema failed";
        }
    }

    if (SUCCEEDED(hr))
    {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
    else if (!error.empty())
    {
        OutputDebugStringW((L"ieColumn: " + error + L"\r\n").c_str());
    }

    if (uninit)
    {
        CoUninitialize();
    }
    return hr;
}

HRESULT UnregisterHandler()
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = coInit == S_OK || coInit == S_FALSE;

    const std::wstring dir = ModuleDirectory();
    if (!dir.empty())
    {
        const std::wstring schemaPath = dir + L"\\" + kPropDescFileName;
        if (GetFileAttributesW(schemaPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            PSUnregisterPropertySchema(schemaPath.c_str());
        }
    }

    wchar_t clsid[64] = {};
    StringFromGUID2(__uuidof(CieColumn), clsid, ARRAYSIZE(clsid));

    std::vector<std::wstring> extensions = ReadMultiSz(HKEY_LOCAL_MACHINE, kConfigRegKey, kRegisteredExtensionsValue);
    if (extensions.empty())
    {
        HandlerConfig config;
        std::wstring error;
        if (SUCCEEDED(LoadHandlerConfigFile(ResolveConfigPath(g_hInst), config, error)))
        {
            extensions = CollectExtensions(config);
        }
    }
    for (const std::wstring& extension : extensions)
    {
        RemoveOwnedExtension(extension, clsid);
    }
    DeleteValue(HKEY_LOCAL_MACHINE, kConfigRegKey, kRegisteredExtensionsValue);

    const std::wstring clsidKey = std::wstring(L"CLSID\\") + clsid;
    RegDeleteTreeW(HKEY_CLASSES_ROOT, clsidKey.c_str());

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (uninit)
    {
        CoUninitialize();
    }
    return S_OK;
}
