#pragma once

#include <propsys.h>
#include <shlwapi.h>

#include <string>

class __declspec(uuid("C13F1FF0-E0D8-4E9C-A21F-F554EB7065E3")) CieColumn
    : public IPropertyStore
    , public IPropertyStoreCapabilities
    , public IInitializeWithFile
{
public:
    CieColumn();

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv);
    IFACEMETHODIMP_(ULONG) AddRef();
    IFACEMETHODIMP_(ULONG) Release();

    IFACEMETHODIMP GetCount(DWORD* cProps);
    IFACEMETHODIMP GetAt(DWORD iProp, PROPERTYKEY* pkey);
    IFACEMETHODIMP GetValue(REFPROPERTYKEY key, PROPVARIANT* pv);
    IFACEMETHODIMP SetValue(REFPROPERTYKEY key, REFPROPVARIANT propvar);
    IFACEMETHODIMP Commit();

    IFACEMETHODIMP IsPropertyWritable(REFPROPERTYKEY key);

    IFACEMETHODIMP Initialize(LPCWSTR pszFilePath, DWORD grfMode);

private:
    ~CieColumn();
    HRESULT InitializeLocked(IStream* pStream, DWORD grfMode, const std::wstring& itemPath);

    long _cRef;
    DWORD _grfMode;
    IPropertyStoreCache* _cache;
    CRITICAL_SECTION _cs;
};

HRESULT CieColumn_CreateInstance(REFIID riid, void** ppv);
