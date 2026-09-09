#include "WMI.h"

#include "log.h"

#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>

#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "wbemuuid.lib")
#endif

static IWbemLocator* g_pLoc = nullptr;
static IWbemServices* g_pSvc = nullptr;

/* Drain for shutdown: _wmishutdown waits until no Indicate is in flight
   before releasing the sink (otherwise Release/CoUninitialize races a
   running callback). */
static volatile LONG g_indicateActive = 0;
static HANDLE g_drainEvent = nullptr; /* manual-reset, signaled when idle */

static void CloseDrainEvent()
{
    if (g_drainEvent) {
        CloseHandle(g_drainEvent);
        g_drainEvent = nullptr;
    }
}

class CWMIEventSink : public IWbemObjectSink {
    LONG m_lRef;
public:
    CWMIEventSink() : m_lRef(0) {}
    virtual ~CWMIEventSink() {}

    virtual ULONG STDMETHODCALLTYPE AddRef() {
        return InterlockedIncrement(&m_lRef);
    }

    virtual ULONG STDMETHODCALLTYPE Release() {
        LONG lRef = InterlockedDecrement(&m_lRef);
        if (lRef == 0)
            delete this;
        return lRef;
    }

    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) {
        if (riid == IID_IUnknown || riid == IID_IWbemObjectSink) {
            *ppv = (IWbemObjectSink*)this;
            AddRef();
            return WBEM_S_NO_ERROR;
        }
        return E_NOINTERFACE;
    }

    virtual HRESULT STDMETHODCALLTYPE Indicate(
        LONG lObjectCount,
        IWbemClassObject** apObjArray
    ) {
        InterlockedIncrement(&g_indicateActive);
        if (g_drainEvent)
            ResetEvent(g_drainEvent);
        for (LONG i = 0; i < lObjectCount; i++) {
            /* Class kind first (cheap single property). */
            VARIANT vtClass;
            VariantInit(&vtClass);
            bool created = false;
            bool known = false;
            if (SUCCEEDED(apObjArray[i]->Get(_bstr_t(L"__Class"), 0, &vtClass, 0, 0))) {
                std::wstring eventClass = vtClass.bstrVal ? vtClass.bstrVal : L"";
                created = (eventClass == L"__InstanceCreationEvent");
                known = created || (eventClass == L"__InstanceDeletionEvent");
                VariantClear(&vtClass);
            }
            if (!known) continue;

            /* Pull the PID out of the embedded TargetInstance — the event
               already knows it; no process-table rescan needed. */
            VARIANT vtTarget;
            VariantInit(&vtTarget);
            if (FAILED(apObjArray[i]->Get(L"TargetInstance", 0, &vtTarget, 0, 0)))
                continue;

            DWORD pid = 0;
            if (vtTarget.vt == VT_UNKNOWN && vtTarget.punkVal) {
                auto* tgt = (IWbemClassObject*)vtTarget.punkVal;
                VARIANT vtPid;
                VariantInit(&vtPid);
                if (SUCCEEDED(tgt->Get(_bstr_t(L"Handle"), 0, &vtPid, 0, 0)) &&
                    vtPid.bstrVal)
                {
                    pid = (DWORD)wcstoul(vtPid.bstrVal, nullptr, 10);
                    VariantClear(&vtPid);
                }
            }
            VariantClear(&vtTarget);

            if (pid)
                TasxNotifyProcess(pid, created ? 1 : 0);
        }
        if (InterlockedDecrement(&g_indicateActive) == 0 && g_drainEvent)
            SetEvent(g_drainEvent);
        return WBEM_S_NO_ERROR;
    }

    virtual HRESULT STDMETHODCALLTYPE SetStatus(
        LONG lFlags,
        HRESULT hResult,
        BSTR strParam,
        IWbemClassObject* pObjParam
    ) {
        (void)strParam;
        (void)pObjParam;
        if (lFlags == WBEM_STATUS_COMPLETE && FAILED(hResult)) {
            LOGW("[WMI] Async query failed. hResult = 0x%08lX", (unsigned long)hResult);
        }
        return WBEM_S_NO_ERROR;
    }
};

static CWMIEventSink* g_pSink = nullptr;

bool _wmimon() {
    HRESULT hr;

    hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        LOGE("[WMI] CoInitializeEx failed. hr = 0x%08lX", (unsigned long)hr);
        return false;
    }

    hr = CoInitializeSecurity(
        NULL,
        -1,
        NULL,
        NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL,
        EOAC_NONE,
        NULL
    );
    if (FAILED(hr)) {
        LOGE("[WMI] CoInitializeSecurity failed. hr = 0x%08lX", (unsigned long)hr);
        CoUninitialize();
        return false;
    }

    hr = CoCreateInstance(
        CLSID_WbemLocator,
        0,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (LPVOID*)&g_pLoc
    );
    if (FAILED(hr)) {
        LOGE("[WMI] CoCreateInstance failed. hr = 0x%08lX", (unsigned long)hr);
        CoUninitialize();
        return false;
    }

    hr = g_pLoc->ConnectServer(
        _bstr_t(L"ROOT\\CIMV2"),
        NULL,
        NULL,
        0,
        0,
        0,
        0,
        &g_pSvc
    );
    if (FAILED(hr)) {
        LOGE("[WMI] ConnectServer failed. hr = 0x%08lX", (unsigned long)hr);
        g_pLoc->Release();
        g_pLoc = nullptr;
        CoUninitialize();
        return false;
    }

    hr = CoSetProxyBlanket(
        g_pSvc,
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        NULL,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL,
        EOAC_NONE
    );
    if (FAILED(hr)) {
        LOGE("[WMI] CoSetProxyBlanket failed. hr = 0x%08lX", (unsigned long)hr);
        g_pSvc->Release();
        g_pSvc = nullptr;
        g_pLoc->Release();
        g_pLoc = nullptr;
        CoUninitialize();
        return false;
    }

    g_pSink = new CWMIEventSink;
    g_pSink->AddRef();

    /* Drain event starts signaled (idle); Indicate resets it while busy. */
    g_drainEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);

    /* Watch both the desktop client and the Store/UWP executable in a
       single query per event kind. */
    hr = g_pSvc->ExecNotificationQueryAsync(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT * FROM __InstanceCreationEvent WITHIN 1 "
            L"WHERE TargetInstance ISA 'Win32_Process' "
            L"AND (TargetInstance.Name = 'RobloxPlayerBeta.exe' "
            L"OR TargetInstance.Name = 'Roblox.exe')"),
        WBEM_FLAG_SEND_STATUS,
        NULL,
        g_pSink
    );
    if (FAILED(hr)) {
        LOGE("[WMI] ExecNotificationQueryAsync (creation) failed. hr = 0x%08lX",
             (unsigned long)hr);
        g_pSink->Release();
        g_pSink = nullptr;
        CloseDrainEvent();
        g_pSvc->Release();
        g_pSvc = nullptr;
        g_pLoc->Release();
        g_pLoc = nullptr;
        CoUninitialize();
        return false;
    }

    hr = g_pSvc->ExecNotificationQueryAsync(
        _bstr_t(L"WQL"),
        _bstr_t(L"SELECT * FROM __InstanceDeletionEvent WITHIN 1 "
            L"WHERE TargetInstance ISA 'Win32_Process' "
            L"AND (TargetInstance.Name = 'RobloxPlayerBeta.exe' "
            L"OR TargetInstance.Name = 'Roblox.exe')"),
        WBEM_FLAG_SEND_STATUS,
        NULL,
        g_pSink
    );
    if (FAILED(hr)) {
        LOGE("[WMI] ExecNotificationQueryAsync (deletion) failed. hr = 0x%08lX",
             (unsigned long)hr);
        g_pSvc->CancelAsyncCall(g_pSink);
        g_pSink->Release();
        g_pSink = nullptr;
        CloseDrainEvent();
        g_pSvc->Release();
        g_pSvc = nullptr;
        g_pLoc->Release();
        g_pLoc = nullptr;
        CoUninitialize();
        return false;
    }

    LOGI("[WMI] Process watcher armed");
    return true;
}

void _wmishutdown() {
    if (g_pSvc && g_pSink) {
        g_pSvc->CancelAsyncCall(g_pSink);
    }

    /* Drain: CancelAsyncCall stops NEW callbacks; wait (bounded) for any
       in-flight Indicate to finish before touching the sink. */
    if (g_drainEvent) {
        WaitForSingleObject(g_drainEvent, 2000);
        CloseDrainEvent();
    }

    if (g_pSink) {
        g_pSink->Release();
        g_pSink = nullptr;
    }

    if (g_pSvc) {
        g_pSvc->Release();
        g_pSvc = nullptr;
    }

    if (g_pLoc) {
        g_pLoc->Release();
        g_pLoc = nullptr;
    }

    CoUninitialize();
}
