#include "Dll.h"

#include "ieColumn.h"
#include "RegisterExtension.h"

#include <new>
#include <shlwapi.h>

namespace
{
    typedef HRESULT (*CreateInstanceFn)(REFIID riid, void** ppv);

    struct ClassObject
    {
        const CLSID* clsid;
        CreateInstanceFn create;
    };

    const ClassObject kClasses[] = {
        { &__uuidof(CieColumn), CieColumn_CreateInstance }
    };

    class CClassFactory : public IClassFactory
    {
    public:
        explicit CClassFactory(CreateInstanceFn create) : _cRef(1), _create(create)
        {
            DllAddRef();
        }

        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv)
        {
            static const QITAB qit[] = {
                QITABENT(CClassFactory, IClassFactory),
                { nullptr }
            };
            return QISearch(this, qit, riid, ppv);
        }

        IFACEMETHODIMP_(ULONG) AddRef()
        {
            return InterlockedIncrement(&_cRef);
        }

        IFACEMETHODIMP_(ULONG) Release()
        {
            const long ref = InterlockedDecrement(&_cRef);
            if (ref == 0)
            {
                delete this;
            }
            return static_cast<ULONG>(ref);
        }

        IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv)
        {
            if (outer != nullptr)
            {
                return CLASS_E_NOAGGREGATION;
            }
            return _create(riid, ppv);
        }

        IFACEMETHODIMP LockServer(BOOL lock)
        {
            if (lock)
            {
                DllAddRef();
            }
            else
            {
                DllRelease();
            }
            return S_OK;
        }

    private:
        ~CClassFactory()
        {
            DllRelease();
        }

        long _cRef;
        CreateInstanceFn _create;
    };
}

HINSTANCE g_hInst = nullptr;
long g_cRefModule = 0;

void DllAddRef()
{
    InterlockedIncrement(&g_cRefModule);
}

void DllRelease()
{
    InterlockedDecrement(&g_cRefModule);
}

STDAPI_(BOOL) DllMain(HINSTANCE instance, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hInst = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow()
{
    return g_cRefModule == 0 ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (ppv == nullptr)
    {
        return E_POINTER;
    }
    *ppv = nullptr;

    for (const ClassObject& entry : kClasses)
    {
        if (clsid == *entry.clsid)
        {
            IClassFactory* factory = new (std::nothrow) CClassFactory(entry.create);
            if (factory == nullptr)
            {
                return E_OUTOFMEMORY;
            }
            const HRESULT hr = factory->QueryInterface(riid, ppv);
            factory->Release();
            return hr;
        }
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllRegisterServer()
{
    return RegisterHandler();
}

STDAPI DllUnregisterServer()
{
    return UnregisterHandler();
}
