#include "ieColumn.h"

#include "BinaryReader.h"
#include "Config.h"
#include "Dll.h"
#include "Tlk.h"

#include <propvarutil.h>

#include <new>
#include <vector>

namespace
{
    template <typename T>
    void SafeRelease(T** pointer)
    {
        if (*pointer)
        {
            (*pointer)->Release();
            *pointer = nullptr;
        }
    }

    constexpr uint64_t kMaxItmBytes = 16ull * 1024ull * 1024ull;
}

CieColumn::CieColumn()
    : _cRef(1)
    , _grfMode(0)
    , _cache(nullptr)
{
    InitializeCriticalSection(&_cs);
    DllAddRef();
}

CieColumn::~CieColumn()
{
    SafeRelease(&_cache);
    DeleteCriticalSection(&_cs);
    DllRelease();
}

IFACEMETHODIMP CieColumn::QueryInterface(REFIID riid, void** ppv)
{
    static const QITAB qit[] = {
        QITABENT(CieColumn, IPropertyStore),
        QITABENT(CieColumn, IPropertyStoreCapabilities),
        QITABENT(CieColumn, IInitializeWithFile),
        { nullptr }
    };
    return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP_(ULONG) CieColumn::AddRef()
{
    return InterlockedIncrement(&_cRef);
}

IFACEMETHODIMP_(ULONG) CieColumn::Release()
{
    const long ref = InterlockedDecrement(&_cRef);
    if (ref == 0)
    {
        delete this;
    }
    return static_cast<ULONG>(ref);
}

IFACEMETHODIMP CieColumn::GetCount(DWORD* cProps)
{
    if (cProps == nullptr)
    {
        return E_POINTER;
    }
    EnterCriticalSection(&_cs);
    const HRESULT hr = _cache ? _cache->GetCount(cProps) : E_UNEXPECTED;
    LeaveCriticalSection(&_cs);
    return hr;
}

IFACEMETHODIMP CieColumn::GetAt(DWORD iProp, PROPERTYKEY* pkey)
{
    if (pkey == nullptr)
    {
        return E_POINTER;
    }
    EnterCriticalSection(&_cs);
    const HRESULT hr = _cache ? _cache->GetAt(iProp, pkey) : E_UNEXPECTED;
    LeaveCriticalSection(&_cs);
    return hr;
}

IFACEMETHODIMP CieColumn::GetValue(REFPROPERTYKEY key, PROPVARIANT* pv)
{
    if (pv == nullptr)
    {
        return E_POINTER;
    }
    PropVariantInit(pv);
    EnterCriticalSection(&_cs);
    const HRESULT hr = _cache ? _cache->GetValue(key, pv) : E_UNEXPECTED;
    LeaveCriticalSection(&_cs);
    return hr;
}

IFACEMETHODIMP CieColumn::SetValue(REFPROPERTYKEY, REFPROPVARIANT)
{
    return STG_E_ACCESSDENIED;
}

IFACEMETHODIMP CieColumn::Commit()
{
    return S_OK;
}

IFACEMETHODIMP CieColumn::IsPropertyWritable(REFPROPERTYKEY)
{
    return S_FALSE;
}

HRESULT ReadStream(IStream* stream, std::vector<uint8_t>& bytes)
{
    bytes.clear();
    LARGE_INTEGER zero = {};
    const HRESULT seekHr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(seekHr))
    {
        return seekHr;
    }

    STATSTG stat = {};
    const HRESULT statHr = stream->Stat(&stat, STATFLAG_NONAME);
    if (SUCCEEDED(statHr) && stat.cbSize.QuadPart > 0)
    {
        if (static_cast<uint64_t>(stat.cbSize.QuadPart) > kMaxItmBytes)
        {
            return E_OUTOFMEMORY;
        }
        bytes.resize(static_cast<size_t>(stat.cbSize.QuadPart));
        ULONG read = 0;
        const HRESULT hr = stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
        if (FAILED(hr))
        {
            return hr;
        }
        bytes.resize(read);
        return S_OK;
    }

    if (FAILED(statHr))
    {
        return statHr;
    }

    uint8_t chunk[64 * 1024];
    for (;;)
    {
        ULONG read = 0;
        const HRESULT hr = stream->Read(chunk, sizeof(chunk), &read);
        if (FAILED(hr))
        {
            return hr;
        }
        if (read == 0)
        {
            break;
        }
        if (bytes.size() + read > kMaxItmBytes)
        {
            return E_OUTOFMEMORY;
        }
        bytes.insert(bytes.end(), chunk, chunk + read);
    }
    return S_OK;
}

HRESULT CieColumn::InitializeLocked(IStream* pStream, DWORD grfMode, const std::wstring& itemPath)
{
    if (_cache != nullptr)
    {
        return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    }
    if (pStream == nullptr || itemPath.empty())
    {
        return E_INVALIDARG;
    }

    _grfMode = grfMode;

    std::vector<uint8_t> bytes;
    HRESULT hr = ReadStream(pStream, bytes);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = PSCreateMemoryPropertyStore(IID_PPV_ARGS(&_cache));
    if (FAILED(hr))
    {
        return hr;
    }

    HandlerConfig config;
    std::wstring error;
    hr = LoadCachedHandlerConfig(g_hInst, config, error);
    if (FAILED(hr))
    {
        OutputDebugStringW((L"ieColumn: " + error + L"\r\n").c_str());
        return S_OK;
    }

    const TypeHandler* handler = FindTypeHandler(config, ExtensionFromPath(itemPath), bytes.data(), bytes.size());
    if (handler == nullptr)
    {
        return S_OK;
    }

    const std::shared_ptr<TlkBytes> tlk = OpenTlkForItem(itemPath);
    StrRefSource strrefs;
    const StrRefSource* strrefSource = nullptr;
    if (tlk != nullptr && tlk->data != nullptr && tlk->size != 0)
    {
        strrefs.data = tlk->data;
        strrefs.size = tlk->size;
        strrefSource = &strrefs;
    }

    for (const ColumnDef& column : handler->columns)
    {
        if (!column.enabled)
        {
            continue;
        }

        EvaluatedValue value;
        if (!EvaluateColumn(bytes.data(), bytes.size(), column, value, strrefSource))
        {
            continue;
        }

        PROPERTYKEY key = {};
        key.fmtid = handler->formatId;
        key.pid = column.propId;

        PROPVARIANT propvar;
        PropVariantInit(&propvar);
        switch (value.kind)
        {
        case EvalKind::Int32:
            hr = InitPropVariantFromInt32(static_cast<LONG>(value.i), &propvar);
            break;
        case EvalKind::UInt32:
            hr = InitPropVariantFromUInt32(static_cast<ULONG>(value.u), &propvar);
            break;
        case EvalKind::String:
            hr = InitPropVariantFromString(value.text.c_str(), &propvar);
            break;
        case EvalKind::Empty:
            hr = S_OK;
            break;
        }
        if (SUCCEEDED(hr) && value.kind != EvalKind::Empty)
        {
            hr = _cache->SetValueAndState(key, &propvar, PSC_READONLY);
        }
        PropVariantClear(&propvar);
        if (FAILED(hr))
        {
            OutputDebugStringW(L"ieColumn: failed to store a column\r\n");
        }
    }

    return S_OK;
}

IFACEMETHODIMP CieColumn::Initialize(LPCWSTR pszFilePath, DWORD grfMode)
{
    if (pszFilePath == nullptr || pszFilePath[0] == L'\0')
    {
        return E_INVALIDARG;
    }

    IStream* stream = nullptr;
    const HRESULT openHr = SHCreateStreamOnFileEx(pszFilePath, STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
    if (FAILED(openHr))
    {
        return openHr;
    }

    EnterCriticalSection(&_cs);
    HRESULT hr = E_FAIL;
    try
    {
        hr = InitializeLocked(stream, grfMode, pszFilePath);
    }
    catch (...)
    {
        hr = E_FAIL;
    }
    if (FAILED(hr))
    {
        SafeRelease(&_cache);
    }
    LeaveCriticalSection(&_cs);
    stream->Release();
    return hr;
}

HRESULT CieColumn_CreateInstance(REFIID riid, void** ppv)
{
    if (ppv == nullptr)
    {
        return E_POINTER;
    }
    *ppv = nullptr;

    CieColumn* handler = new (std::nothrow) CieColumn();
    if (handler == nullptr)
    {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = handler->QueryInterface(riid, ppv);
    handler->Release();
    return hr;
}
