// ============================================================================
//  dinput8_fix.dll  -  DirectInput8 proxy for GOD EATER RESURRECTION (x86)
//
//  Root cause this fixes
//  ---------------------
//  GER.exe (32-bit, engine "ix") reads controllers through DirectInput8
//  (QDirectInputPad / io::Controller); XInput is only used for vibration
//  (XInputSetState / XInputGetCapabilities).
//  A DirectInput device object is bound to a physical device instance.  When
//  the USB pad is unplugged, that object becomes a permanent orphan: every
//  later Poll()/GetDeviceState() returns DIERR_INPUTLOST / DIERR_NOTACQUIRED /
//  DIERR_UNPLUGGED, and even when the pad is plugged back in the game keeps
//  hammering the dead object -> "controller stops working until restart".
//
//  What this proxy does
//  --------------------
//  * wraps IDirectInput8A/W + IDirectInputDevice8A/W
//  * on any device I/O error it re-acquires, and if that is not enough it
//    re-enumerates, re-creates and re-applies (data format, properties,
//    cooperative level, event notification, acquire) the very same device
//  * a watchdog thread keeps doing that even if the game stops polling
//  * optional keep-alive: while the pad is away a neutral (centered, no
//    buttons) state is reported so the game never latches "no controller"
//  * the GECV mod loader from the chinese patch is renamed to GECV_LOADER.dll
//    and is loaded by this proxy, so the translation still works
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <intrin.h>
#include <dinput.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
//  globals / config
// ---------------------------------------------------------------------------
static HMODULE          g_self = NULL;
static WCHAR            g_dir[MAX_PATH] = { 0 };
static CRITICAL_SECTION g_csLog;
static CRITICAL_SECTION g_csDev;
static CRITICAL_SECTION g_csWatch;
static FILE*            g_log = NULL;
static bool             g_logOk = false;
static HANDLE           g_watchThread = NULL;
static volatile LONG    g_stop = 0;

static int cfg_log = 1;
static int cfg_keepAlive = 1;
static int cfg_discMode = 0;      // 0 neutral, 1 last state, 2 error
static int cfg_watchdog = 1;
static int cfg_rebindMs = 400;
static int cfg_heartbeatSec = 5;
static int cfg_verbose = 0;

static int      cfg_borderless = 1;
static int      cfg_toggleVk = 0x7A;   // VK_F11
static int      cfg_dpiAware = 1;
static int      cfg_blockDevChange = 1;
static bool     g_borderOn = true;
static HWND     g_gameWnd = NULL;
static void SubclassGameWindow(HWND h);
static WNDPROC  g_oldWndProc = NULL;
static BOOL     g_wndSubclassed = FALSE;

typedef HRESULT(WINAPI* PFN_DI8CREATE)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static HMODULE       g_realDll = NULL;
static PFN_DI8CREATE g_realCreate = NULL;

static HANDLE           g_crashFile = INVALID_HANDLE_VALUE;
static int              cfg_diag = 1;

typedef USHORT(WINAPI* PFN_STACKWALK)(ULONG, ULONG, PVOID*, PULONG);
static PFN_STACKWALK   g_stackWalk = NULL;

static void CrashLog(const char* s)
{
    if (g_crashFile == INVALID_HANDLE_VALUE) {
        WCHAR p[MAX_PATH];
        swprintf_s(p, L"%s\\\\dinput8_crash.log", g_dir);
        g_crashFile = CreateFileW(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_crashFile == INVALID_HANDLE_VALUE) return;
    }
    DWORD w = 0;
    WriteFile(g_crashFile, s, (DWORD)strlen(s), &w, NULL);
}

static LONG CALLBACK VexHandler(PEXCEPTION_POINTERS p)
{
    DWORD code = p->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;

    char buf[600];
    HMODULE gameBase = GetModuleHandleW(NULL);
    sprintf_s(buf, "=== VEH code=%08lX at %p (self=%p game=%p tid=%lu) access=",
        (unsigned long)code, p->ExceptionRecord->ExceptionAddress, (void*)g_self, (void*)gameBase, GetCurrentThreadId());
    CrashLog(buf);
    if (p->ExceptionRecord->NumberParameters >= 2) {
        sprintf_s(buf, "%lu addr=%p\r\n",
            (unsigned long)p->ExceptionRecord->ExceptionInformation[0],
            (void*)p->ExceptionRecord->ExceptionInformation[1]);
    } else {
        sprintf_s(buf, "n/a\r\n");
    }
    CrashLog(buf);

    if (g_stackWalk) {
        void* frames[40];
        USHORT n = g_stackWalk(0, 40, frames, NULL);
        for (USHORT i = 0; i < n; ++i) {
            HMODULE m = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCWSTR)frames[i], &m);
            char nm[MAX_PATH] = "?";
            if (m) GetModuleFileNameA(m, nm, MAX_PATH);
            const char* b = strrchr(nm, '\\');
            b = b ? b + 1 : nm;
            sprintf_s(buf, "  #%02d %p  %s+0x%X\r\n", (int)i, frames[i], b,
                m ? (unsigned)((BYTE*)frames[i] - (BYTE*)m) : 0u);
            CrashLog(buf);
        }
    }
    FlushFileBuffers(g_crashFile);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InitDiagnostics()
{
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) nt = LoadLibraryW(L"ntdll.dll");
    if (nt) g_stackWalk = (PFN_STACKWALK)GetProcAddress(nt, "RtlCaptureStackBackTrace");
    AddVectoredExceptionHandler(1, VexHandler);
}

static void LogF(const char* fmt, ...)
{
    if (!g_logOk || !g_log) return;
    EnterCriticalSection(&g_csLog);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_csLog);
}

static const char* HrName(HRESULT hr)
{
    switch (hr) {
    case DI_OK:                return "DI_OK";
    case DI_NOEFFECT:          return "DI_NOEFFECT";
    case DIERR_INPUTLOST:      return "DIERR_INPUTLOST";
    case DIERR_NOTACQUIRED:    return "DIERR_NOTACQUIRED";
    case DIERR_UNPLUGGED:      return "DIERR_UNPLUGGED";
    case DIERR_NOTINITIALIZED: return "DIERR_NOTINITIALIZED";
    case DIERR_NOTFOUND:       return "DIERR_NOTFOUND";
    case DIERR_ACQUIRED:       return "DIERR_ACQUIRED";
    case DIERR_DEVICENOTREG:   return "DIERR_DEVICENOTREG";
    case E_NOINTERFACE:        return "E_NOINTERFACE";
    case E_FAIL:               return "E_FAIL";
    case E_NOTIMPL:            return "E_NOTIMPL";
    default:                   return "hr?";
    }
}

static std::string GuidStr(const GUID& g)
{
    char b[64];
    sprintf_s(b, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return std::string(b);
}

static bool IsDeviceLostHr(HRESULT hr)
{
    return hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED ||
           hr == DIERR_UNPLUGGED || hr == DIERR_NOTINITIALIZED ||
           hr == DIERR_NOTFOUND || hr == E_ACCESSDENIED;
}

// ---------------------------------------------------------------------------
//  device bookkeeping (variant independent)
// ---------------------------------------------------------------------------
struct PropRec
{
    UINT_PTR          guid;      // REFGUID *value*: may be a MAKEDIPROP id (e.g. 4)
    std::vector<BYTE> data;
};

struct RangeRec
{
    DWORD dwObj;
    DWORD dwHow;
    LONG  lMin;
    LONG  lMax;
};

struct DevCtx
{
    GUID  instGUID;
    GUID  prodGUID;
    bool  haveInfo;
    DWORD devType;
    char  instNameA[MAX_PATH];
    WCHAR instNameW[MAX_PATH];
    char  prodNameA[MAX_PATH];
    WCHAR prodNameW[MAX_PATH];

    DIDATAFORMAT                    df;
    std::vector<DIOBJECTDATAFORMAT>  dfObj;
    bool                            hasFormat;

    std::vector<PropRec>  props;
    std::vector<RangeRec> ranges;

    HWND   hwnd;
    DWORD  coop;
    HANDLE evt;

    bool acquiredEver;
    bool gameAcquired;
    bool disconnected;

    DWORD lastRepairTick;
    DWORD lastFailLog;
    DWORD reads, errors, repairs, rebinds, fails;

    std::vector<BYTE> lastGood;
    DIDEVCAPS caps;
    bool      haveCaps;

    DevCtx()
        : haveInfo(false), devType(0), hasFormat(false), hwnd(NULL), coop(0), evt(NULL),
          acquiredEver(false), gameAcquired(false), disconnected(false), lastRepairTick(0), lastFailLog(0),
          reads(0), errors(0), repairs(0), rebinds(0), fails(0), haveCaps(false)
    {
        memset(&instGUID, 0, sizeof(instGUID));
        memset(&prodGUID, 0, sizeof(prodGUID));
        memset(instNameA, 0, sizeof(instNameA));
        memset(instNameW, 0, sizeof(instNameW));
        memset(prodNameA, 0, sizeof(prodNameA));
        memset(prodNameW, 0, sizeof(prodNameW));
        memset(&df, 0, sizeof(df));
        memset(&caps, 0, sizeof(caps));
    }
};

// name helpers (overloads resolve for the A / W variant statically)
static int NameCmp(const char* a, const char* b)  { return lstrcmpiA(a, b); }
static int NameCmp(const WCHAR* a, const WCHAR* b) { return lstrcmpiW(a, b); }
static const char*  InstNameOf(const DevCtx* c, char*)  { return c->instNameA; }
static const WCHAR* InstNameOf(const DevCtx* c, WCHAR*) { return c->instNameW; }

static void StoreNames(DevCtx* c, const char* ia, const char* pa)
{
    strcpy_s(c->instNameA, MAX_PATH, ia);
    strcpy_s(c->prodNameA, MAX_PATH, pa);
    MultiByteToWideChar(CP_ACP, 0, ia, -1, c->instNameW, MAX_PATH);
    MultiByteToWideChar(CP_ACP, 0, pa, -1, c->prodNameW, MAX_PATH);
}
static void StoreNames(DevCtx* c, const WCHAR* iw, const WCHAR* pw)
{
    wcscpy_s(c->instNameW, MAX_PATH, iw);
    wcscpy_s(c->prodNameW, MAX_PATH, pw);
    WideCharToMultiByte(CP_ACP, 0, iw, -1, c->instNameA, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, pw, -1, c->prodNameA, MAX_PATH, NULL, NULL);
}
static void LoadNames(const DevCtx* c, char* ia, char* pa)
{
    strcpy_s(ia, MAX_PATH, c->instNameA);
    strcpy_s(pa, MAX_PATH, c->prodNameA);
}
static void LoadNames(const DevCtx* c, WCHAR* iw, WCHAR* pw)
{
    wcscpy_s(iw, MAX_PATH, c->instNameW);
    wcscpy_s(pw, MAX_PATH, c->prodNameW);
}

// DIPROP_RANGE lives in the GUID family {000101xx-0000-0000-c000-...-0046},
// but DirectInput also accepts MAKEDIPROP(4) == (REFGUID)4, which is what most
// games pass.  Never dereference such a "property id".
static bool LooksLikeRangeGuid(const GUID& g)
{
    if (g.Data2 != 0 || g.Data3 != 0) return false;
    if (g.Data1 < 0x00010100 || g.Data1 > 0x000101FF) return false;
    return g.Data4[0] == 0xC0 && g.Data4[7] == 0x46;
}

static bool IsRangeProp(REFGUID g)
{
    UINT_PTR v = (UINT_PTR)(const void*)&g;
    if (v < 0x10000) return v == 4;              // DIPROP_RANGE
    return LooksLikeRangeGuid(g);
}

// ---------------------------------------------------------------------------
//  neutral joystick state
// ---------------------------------------------------------------------------
static void FillNeutral(DevCtx* c, void* buf, DWORD cb)
{
    if (!buf || !cb) return;
    memset(buf, 0, cb);
    if (!c->hasFormat) return;

    const DIDATAFORMAT& f = c->df;
    for (DWORD i = 0; i < f.dwNumObjs; ++i) {
        const DIOBJECTDATAFORMAT& o = c->dfObj[i];
        if (o.dwOfs >= cb) continue;
        DWORD t = o.dwType;

        if (t & (DIDFT_ABSAXIS | DIDFT_RELAXIS)) {
            LONG lo = 0, hi = 65535; bool has = false;
            for (size_t r = 0; r < c->ranges.size(); ++r) {
                const RangeRec& rr = c->ranges[r];
                if (rr.dwHow == DIPH_BYOFFSET && rr.dwObj == o.dwOfs) { lo = rr.lMin; hi = rr.lMax; has = true; break; }
                if (rr.dwHow == DIPH_BYID && (rr.dwObj & 0xFFFF) == o.dwOfs) { lo = rr.lMin; hi = rr.lMax; has = true; break; }
            }
            LONG mid = (t & DIDFT_RELAXIS) ? 0 : (has ? (lo + (hi - lo) / 2) : 32767);
            if (o.dwOfs + 4 <= cb) *(DWORD*)((BYTE*)buf + o.dwOfs) = (DWORD)mid;
        }
        else if (t & DIDFT_POV) {
            if (o.dwOfs + 4 <= cb) *(DWORD*)((BYTE*)buf + o.dwOfs) = 0xFFFFFFFF;
        }
        else if (o.dwOfs + 1 <= cb) {
            *((BYTE*)buf + o.dwOfs) = 0;
        }
    }
}

// ---------------------------------------------------------------------------
//  variant traits
// ---------------------------------------------------------------------------
struct VA
{
    typedef char                          CH;
    typedef IDirectInput8A                I8;
    typedef IDirectInputDevice8A          Dev;
    typedef DIDEVICEINSTANCEA             Inst;
    typedef DIDEVICEOBJECTINSTANCEA       ObjInst;
    typedef LPDIENUMDEVICESCALLBACKA      DevCb;
    typedef LPDIENUMDEVICEOBJECTSCALLBACKA ObjCb;
    typedef LPDIENUMEFFECTSCALLBACKA      EffCb;
    typedef DIEFFECTINFOA                 EffInfo;
    typedef DIACTIONFORMATA               ActMap;
    typedef DIDEVICEIMAGEINFOHEADERA      ImgHdr;
    typedef LPDIENUMEFFECTSINFILECALLBACK FileEffCb;
    typedef LPDIENUMDEVICESBYSEMANTICSCBA SemCb;
    typedef DICONFIGUREDEVICESPARAMSA     CfgParams;
    typedef LPCSTR                        Str;

    static REFIID Iid()    { return IID_IDirectInput8A; }
    static REFIID DevIid() { return IID_IDirectInputDevice8A; }
    static HRESULT CreateDevice(I8* di, REFGUID g, Dev** out) { return di->CreateDevice(g, out, NULL); }
};

struct VW
{
    typedef WCHAR                         CH;
    typedef IDirectInput8W                I8;
    typedef IDirectInputDevice8W          Dev;
    typedef DIDEVICEINSTANCEW             Inst;
    typedef DIDEVICEOBJECTINSTANCEW       ObjInst;
    typedef LPDIENUMDEVICESCALLBACKW      DevCb;
    typedef LPDIENUMDEVICEOBJECTSCALLBACKW ObjCb;
    typedef LPDIENUMEFFECTSCALLBACKW      EffCb;
    typedef DIEFFECTINFOW                 EffInfo;
    typedef DIACTIONFORMATW               ActMap;
    typedef DIDEVICEIMAGEINFOHEADERW      ImgHdr;
    typedef LPDIENUMEFFECTSINFILECALLBACK FileEffCb;
    typedef LPDIENUMDEVICESBYSEMANTICSCBW SemCb;
    typedef DICONFIGUREDEVICESPARAMSW     CfgParams;
    typedef LPCWSTR                        Str;

    static REFIID Iid()    { return IID_IDirectInput8W; }
    static REFIID DevIid() { return IID_IDirectInputDevice8W; }
    static HRESULT CreateDevice(I8* di, REFGUID g, Dev** out) { return di->CreateDevice(g, out, NULL); }
};

// ---------------------------------------------------------------------------
//  watchdog interface
// ---------------------------------------------------------------------------
class IWatch
{
public:
    virtual void  WatchTick() = 0;
    virtual ULONG WatchAddRef() = 0;
    virtual ULONG WatchRelease() = 0;
    virtual void  Stats(char* out, int n) = 0;
    virtual ~IWatch() {}
};

static std::vector<IWatch*> g_watch;
static std::vector<GUID>    g_knownGuids;   // every pad GUID we ever saw

static void RememberGuid(const GUID& g)
{
    EnterCriticalSection(&g_csWatch);
    for (size_t i = 0; i < g_knownGuids.size(); ++i)
        if (IsEqualGUID(g_knownGuids[i], g)) { LeaveCriticalSection(&g_csWatch); return; }
    g_knownGuids.push_back(g);
    LeaveCriticalSection(&g_csWatch);
}

static bool IsKnownGuid(REFGUID g)
{
    bool f = false;
    EnterCriticalSection(&g_csWatch);
    for (size_t i = 0; i < g_knownGuids.size(); ++i)
        if (IsEqualGUID(g_knownGuids[i], g)) { f = true; break; }
    LeaveCriticalSection(&g_csWatch);
    return f;
}

// ---------------------------------------------------------------------------
//  device proxy
// ---------------------------------------------------------------------------
template <class V>
class TDevice : public V::Dev, public IWatch
{
    typedef typename V::Dev      Dev;
    typedef typename V::I8       I8;
    typedef typename V::Inst     Inst;
    typedef typename V::ObjInst  ObjInst;
    typedef typename V::DevCb    DevCb;
    typedef typename V::ObjCb    ObjCb;
    typedef typename V::EffCb    EffCb;
    typedef typename V::EffInfo  EffInfo;
    typedef typename V::ActMap   ActMap;
    typedef typename V::ImgHdr   ImgHdr;
    typedef typename V::FileEffCb FileEffCb;
    typedef typename V::CH       CH;

public:
    TDevice(Dev* real, I8* parent, REFGUID requested)
        : m_real(real), m_parent(parent), m_ref(1), m_ctx(new DevCtx()), m_id(0)
    {
        if (m_parent) m_parent->AddRef();
        if (m_real) m_real->AddRef();
        m_ctx->instGUID = requested;
        m_id = (DWORD)(ULONG_PTR)this;
        RefreshInfo();
        RememberGuid(m_ctx->instGUID);
        WatchRegister();
    }

    virtual ~TDevice()
    {
        if (m_real) m_real->Release();
        for (size_t i = 0; i < m_retired.size(); ++i) m_retired[i]->Release();
        if (m_parent) m_parent->Release();
        delete m_ctx;
    }

    DevCtx* Ctx() { return m_ctx; }

    // ---------------- IUnknown ----------------
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, V::DevIid())) {
            AddRef();
            *ppv = static_cast<Dev*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) {
            EnterCriticalSection(&g_csWatch);
            for (size_t i = 0; i < g_watch.size(); ++i)
                if (g_watch[i] == this) { g_watch.erase(g_watch.begin() + i); break; }
            LeaveCriticalSection(&g_csWatch);
            delete this;
        }
        return (ULONG)r;
    }
    ULONG WatchAddRef() override { return AddRef(); }
    ULONG WatchRelease() override { return Release(); }

    // ---------------- IDirectInputDevice8 ----------------
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS p) override
    {
        HRESULT hr = m_real ? m_real->GetCapabilities(p) : E_FAIL;
        if (SUCCEEDED(hr) && p) { m_ctx->caps = *p; m_ctx->haveCaps = true; }
        else if (m_ctx->haveCaps && p) { *p = m_ctx->caps; return DI_OK; }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(ObjCb cb, LPVOID ref, DWORD flags) override
    { return m_real ? m_real->EnumObjects(cb, ref, flags) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID g, LPDIPROPHEADER h) override
    { return m_real ? m_real->GetProperty(g, h) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID g, LPCDIPROPHEADER h) override
    {
        HRESULT hr = m_real ? m_real->SetProperty(g, h) : E_FAIL;
        if (SUCCEEDED(hr) && h) RecordProp(g, h);
        if (cfg_verbose) LogF("dev#%08X SetProperty g=0x%08lX (%s) how=%lu size=%lu -> %s", m_id,
            (unsigned long)(UINT_PTR)(const void*)&g,
            ((UINT_PTR)(const void*)&g < 0x10000) ? "propid" : "guid",
            (h && !IsBadReadPtr(h, 16)) ? (unsigned long)h->dwHow : 0,
            (h && !IsBadReadPtr(h, 16)) ? (unsigned long)h->dwSize : 0,
            HrName(hr));
        return hr;
    }

    HRESULT STDMETHODCALLTYPE Acquire() override
    {
        HRESULT hr = m_real ? m_real->Acquire() : E_FAIL;
        if (SUCCEEDED(hr)) {
            m_ctx->acquiredEver = true;
            m_ctx->gameAcquired = true;
            m_ctx->disconnected = false;
            WatchRegister();
            if (cfg_verbose) LogF("dev#%08X Acquire -> DI_OK", m_id);
        }
        else if (cfg_keepAlive && IsDeviceLostHr(hr)) {
            LogF("dev#%08X Acquire failed (%s) -> keep-alive", m_id, HrName(hr));
            m_ctx->gameAcquired = true;
            WatchRegister();
            Repair();
            return DI_OK;
        }
        else LogF("dev#%08X Acquire failed (%s)", m_id, HrName(hr));
        return hr;
    }

    virtual void Stats(char* out, int n) override
    {
        _snprintf_s(out, n, _TRUNCATE,
            "dev#%08X %-46s reads=%lu errs=%lu rep=%lu reb=%lu acq=%d disc=%d",
            m_id, m_ctx->prodNameA[0] ? m_ctx->prodNameA : "?",
            (unsigned long)m_ctx->reads, (unsigned long)m_ctx->errors,
            (unsigned long)m_ctx->repairs, (unsigned long)m_ctx->rebinds,
            m_ctx->gameAcquired ? 1 : 0, m_ctx->disconnected ? 1 : 0);
    }

    HRESULT STDMETHODCALLTYPE Unacquire() override
    {
        m_ctx->gameAcquired = false;
        return m_real ? m_real->Unacquire() : DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID pv) override
    {
        InterlockedIncrement((LONG*)&m_ctx->reads);
        HRESULT hr = m_real ? m_real->GetDeviceState(cb, pv) : E_FAIL;
        if (SUCCEEDED(hr)) {
            if (m_ctx->disconnected) { LogF("dev#%08X input is back", m_id); m_ctx->disconnected = false; }
            m_ctx->acquiredEver = true;
            if (!m_ctx->gameAcquired) { m_ctx->gameAcquired = true; WatchRegister(); }
            SaveLast(pv, cb);
            return hr;
        }
        if (!IsDeviceLostHr(hr)) return hr;

        InterlockedIncrement((LONG*)&m_ctx->errors);
        if (cfg_verbose) LogF("dev#%08X GetDeviceState -> %s", m_id, HrName(hr));

        if (Repair()) {
            HRESULT hr2 = m_real ? m_real->GetDeviceState(cb, pv) : E_FAIL;
            if (SUCCEEDED(hr2)) { SaveLast(pv, cb); return hr2; }
        }
        if (!cfg_keepAlive) return hr;

        if (!m_ctx->disconnected) {
            LogF("dev#%08X pad lost -> keep-alive, reporting neutral input", m_id);
            m_ctx->disconnected = true;
        }
        if (cfg_discMode == 1 && m_ctx->lastGood.size() >= cb) memcpy(pv, m_ctx->lastGood.data(), cb);
        else FillNeutral(m_ctx, pv, cb);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cbObj, LPDIDEVICEOBJECTDATA rg, LPDWORD pInOut, DWORD flags) override
    {
        HRESULT hr = m_real ? m_real->GetDeviceData(cbObj, rg, pInOut, flags) : E_FAIL;
        if (SUCCEEDED(hr)) return hr;
        if (!IsDeviceLostHr(hr)) return hr;
        InterlockedIncrement((LONG*)&m_ctx->errors);
        if (Repair()) {
            HRESULT hr2 = m_real ? m_real->GetDeviceData(cbObj, rg, pInOut, flags) : E_FAIL;
            if (SUCCEEDED(hr2)) return hr2;
        }
        if (!cfg_keepAlive) return hr;
        if (pInOut) *pInOut = 0;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT pdf) override
    {
        HRESULT hr = m_real ? m_real->SetDataFormat(pdf) : E_FAIL;
        if (SUCCEEDED(hr) && pdf) StoreFormat(pdf);
        if (cfg_verbose) LogF("dev#%08X SetDataFormat objs=%lu dataSize=%lu -> %s", m_id,
            pdf ? pdf->dwNumObjs : 0, pdf ? pdf->dwDataSize : 0, HrName(hr));
        return hr;
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE h) override
    {
        m_ctx->evt = h;
        return m_real ? m_real->SetEventNotification(h) : DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND w, DWORD f) override
    {
        m_ctx->hwnd = w; m_ctx->coop = f;
        if (cfg_verbose) LogF("dev#%08X SetCooperativeLevel hwnd=%p flags=0x%08lX", m_id, (void*)w, f);
        return m_real ? m_real->SetCooperativeLevel(w, f) : DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(ObjInst* p, DWORD obj, DWORD how) override
    { return m_real ? m_real->GetObjectInfo(p, obj, how) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(Inst* p) override
    {
        HRESULT hr = m_real ? m_real->GetDeviceInfo(p) : E_FAIL;
        if (SUCCEEDED(hr) && p) CacheInfo(p);
        else if (m_ctx->haveInfo && p) { RebuildInfo(p); return DI_OK; }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND w, DWORD f) override
    { return m_real ? m_real->RunControlPanel(w, f) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v, REFGUID g) override
    { return m_real ? m_real->Initialize(h, v, g) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* o, LPUNKNOWN u) override
    { return m_real ? m_real->CreateEffect(g, e, o, u) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE EnumEffects(EffCb cb, LPVOID r, DWORD f) override
    { return m_real ? m_real->EnumEffects(cb, r, f) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE GetEffectInfo(EffInfo* i, REFGUID g) override
    { return m_real ? m_real->GetEffectInfo(i, g) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD pdw) override
    { return m_real ? m_real->GetForceFeedbackState(pdw) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD f) override
    { return m_real ? m_real->SendForceFeedbackCommand(f) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, LPVOID r, DWORD f) override
    { return m_real ? m_real->EnumCreatedEffectObjects(cb, r, f) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE e) override
    { return m_real ? m_real->Escape(e) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE Poll() override
    {
        HRESULT hr = m_real ? m_real->Poll() : E_FAIL;
        if (SUCCEEDED(hr) || hr == DI_NOEFFECT) return DI_OK;
        if (!IsDeviceLostHr(hr)) return hr;
        InterlockedIncrement((LONG*)&m_ctx->errors);
        if (Repair()) {
            HRESULT hr2 = m_real ? m_real->Poll() : E_FAIL;
            if (SUCCEEDED(hr2) || hr2 == DI_NOEFFECT) return DI_OK;
        }
        if (cfg_keepAlive) return DI_OK;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD cb, LPCDIDEVICEOBJECTDATA d, LPDWORD pInOut, DWORD f) override
    { return m_real ? m_real->SendDeviceData(cb, d, pInOut, f) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(typename V::Str fn, FileEffCb cb, LPVOID r, DWORD f) override
    { return m_real ? m_real->EnumEffectsInFile(fn, cb, r, f) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE WriteEffectToFile(typename V::Str fn, DWORD n, LPDIFILEEFFECT e, DWORD f) override
    { return m_real ? m_real->WriteEffectToFile(fn, n, e, f) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE BuildActionMap(ActMap* a, typename V::Str usr, DWORD f) override
    { return m_real ? m_real->BuildActionMap(a, usr, f) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE SetActionMap(ActMap* a, typename V::Str usr, DWORD f) override
    { return m_real ? m_real->SetActionMap(a, usr, f) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE GetImageInfo(ImgHdr* i) override
    { return m_real ? m_real->GetImageInfo(i) : E_FAIL; }

    // ---------------- watchdog ----------------
    virtual void WatchTick() override
    {
        if (!m_ctx->gameAcquired && !m_ctx->disconnected) return;
        Dev* d = m_real;
        if (!d) return;
        HRESULT hr = d->Poll();
        if (SUCCEEDED(hr) || hr == DI_NOEFFECT) {
            if (m_ctx->disconnected) {
                // the device object survived; make sure it is acquired again
                if (SUCCEEDED(d->Acquire())) { m_ctx->disconnected = false; LogF("dev#%08X watchdog: re-acquired", m_id); }
            }
            return;
        }
        if (!IsDeviceLostHr(hr)) return;
        LogF("dev#%08X watchdog: device lost (%s)", m_id, HrName(hr));
        InterlockedIncrement((LONG*)&m_ctx->errors);
        Repair();
    }

private:
    Dev*   m_real;
    I8*    m_parent;
    LONG   m_ref;
    DevCtx* m_ctx;
    DWORD  m_id;
    std::vector<Dev*> m_retired;

    void WatchRegister()
    {
        if (!cfg_watchdog) return;
        EnterCriticalSection(&g_csWatch);
        bool dup = false;
        for (size_t i = 0; i < g_watch.size(); ++i) if (g_watch[i] == this) { dup = true; break; }
        if (!dup) g_watch.push_back(this);
        LeaveCriticalSection(&g_csWatch);
    }

    void SaveLast(const void* pv, DWORD cb)
    {
        if (!pv || !cb || cb > 8192) return;
        m_ctx->lastGood.resize(cb);
        memcpy(m_ctx->lastGood.data(), pv, cb);
    }

    void StoreFormat(LPCDIDATAFORMAT pdf)
    {
        EnterCriticalSection(&g_csDev);
        if (pdf && pdf->dwNumObjs && pdf->rgodf) {
            m_ctx->df = *pdf;
            m_ctx->dfObj.assign(pdf->rgodf, pdf->rgodf + pdf->dwNumObjs);
            m_ctx->df.rgodf = m_ctx->dfObj.data();
            m_ctx->hasFormat = true;
        }
        LeaveCriticalSection(&g_csDev);
    }

    void RecordProp(REFGUID g, LPCDIPROPHEADER h)
    {
        if (h->dwSize < sizeof(DIPROPHEADER)) return;
        EnterCriticalSection(&g_csDev);
        PropRec r;
        r.guid = (UINT_PTR)(const void*)&g;
        r.data.assign((const BYTE*)h, (const BYTE*)h + h->dwSize);
        bool replaced = false;
        for (size_t i = 0; i < m_ctx->props.size(); ++i) {
            PropRec& e = m_ctx->props[i];
            if (e.guid == r.guid && e.data.size() == r.data.size()) {
                LPCDIPROPHEADER eh = (LPCDIPROPHEADER)e.data.data();
                if (eh->dwObj == h->dwObj && eh->dwHow == h->dwHow) { e = r; replaced = true; break; }
            }
        }
        if (!replaced) m_ctx->props.push_back(r);

        if (IsRangeProp(g) && h->dwSize >= sizeof(DIPROPRANGE)) {
            const DIPROPRANGE* pr = (const DIPROPRANGE*)h;
            RangeRec rr; rr.dwObj = h->dwObj; rr.dwHow = h->dwHow; rr.lMin = pr->lMin; rr.lMax = pr->lMax;
            bool rep = false;
            for (size_t i = 0; i < m_ctx->ranges.size(); ++i)
                if (m_ctx->ranges[i].dwObj == rr.dwObj && m_ctx->ranges[i].dwHow == rr.dwHow) { m_ctx->ranges[i] = rr; rep = true; break; }
            if (!rep) m_ctx->ranges.push_back(rr);
        }
        LeaveCriticalSection(&g_csDev);
    }

    void CacheInfo(const Inst* p)
    {
        EnterCriticalSection(&g_csDev);
        m_ctx->instGUID = p->guidInstance;
        m_ctx->prodGUID = p->guidProduct;
        m_ctx->devType  = p->dwDevType;
        StoreNames(m_ctx, p->tszInstanceName, p->tszProductName);
        m_ctx->haveInfo = true;
        LeaveCriticalSection(&g_csDev);
    }

    void RebuildInfo(Inst* p)
    {
        EnterCriticalSection(&g_csDev);
        memset(p, 0, sizeof(Inst));
        p->dwSize = sizeof(Inst);
        p->guidInstance = m_ctx->instGUID;
        p->guidProduct  = m_ctx->prodGUID;
        p->dwDevType    = m_ctx->devType;
        LoadNames(m_ctx, p->tszInstanceName, p->tszProductName);
        LeaveCriticalSection(&g_csDev);
    }

    void RefreshInfo()
    {
        if (!m_real) return;
        Inst di; memset(&di, 0, sizeof(di)); di.dwSize = sizeof(di);
        if (SUCCEEDED(m_real->GetDeviceInfo(&di))) CacheInfo(&di);
        DIDEVCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
        if (SUCCEEDED(m_real->GetCapabilities(&c))) { m_ctx->caps = c; m_ctx->haveCaps = true; }
    }

    // ---- enumerate for the same physical device -------------------------
    struct FindCtx
    {
        const GUID* byInst;
        const GUID* byProd;
        const CH*   name;
        bool        needName;
        bool        match;
        GUID        found;
    };

    static BOOL FAR PASCAL FindCb(const Inst* inst, LPVOID ref)
    {
        FindCtx* f = (FindCtx*)ref;
        if (!inst) return DIENUM_CONTINUE;
        if (f->byInst && inst->guidInstance == *f->byInst) {
            f->found = inst->guidInstance; f->match = true; return DIENUM_STOP;
        }
        if (f->byProd && inst->guidProduct == *f->byProd) {
            if (!f->needName || (f->name && inst->tszInstanceName &&
                NameCmp(inst->tszInstanceName, f->name) == 0)) {
                f->found = inst->guidInstance; f->match = true; return DIENUM_STOP;
            }
        }
        return DIENUM_CONTINUE;
    }

    bool FindDevice(I8* di, GUID* out)
    {
        FindCtx f;
        memset(&f, 0, sizeof(f));
        f.byInst = &m_ctx->instGUID;
        di->EnumDevices(DI8DEVCLASS_ALL, (DevCb)&FindCb, &f, DIEDFL_ATTACHEDONLY);
        if (f.match) { *out = f.found; return true; }

        if (m_ctx->haveInfo) {
            memset(&f, 0, sizeof(f));
            f.byProd = &m_ctx->prodGUID; f.needName = true; f.name = InstNameOf(m_ctx, (CH*)NULL);
            di->EnumDevices(DI8DEVCLASS_ALL, (DevCb)&FindCb, &f, DIEDFL_ATTACHEDONLY);
            if (f.match) { *out = f.found; return true; }

            memset(&f, 0, sizeof(f));
            f.byProd = &m_ctx->prodGUID;
            di->EnumDevices(DI8DEVCLASS_ALL, (DevCb)&FindCb, &f, DIEDFL_ATTACHEDONLY);
            if (f.match) { *out = f.found; return true; }
        }
        return false;
    }

    bool Repair()
    {
        I8* parent = m_parent;
        Dev* real = m_real;
        if (!parent || !real) return false;

        DWORD now = GetTickCount();
        if (m_ctx->lastRepairTick && (now - m_ctx->lastRepairTick) < (DWORD)cfg_rebindMs) return false;

        EnterCriticalSection(&g_csDev);      // recursive: serialises game thread + watchdog
        now = GetTickCount();
        if (m_ctx->lastRepairTick && (now - m_ctx->lastRepairTick) < (DWORD)cfg_rebindMs) {
            LeaveCriticalSection(&g_csDev);
            return false;
        }
        m_ctx->lastRepairTick = now ? now : 1;

        // 1) same object might just need a re-acquire
        HRESULT hp = real->Poll();
        if (hp == DI_OK || hp == DI_NOEFFECT || hp == DIERR_NOTACQUIRED) {
            HRESULT ha = real->Acquire();
            if (SUCCEEDED(ha)) {
                m_ctx->repairs++;
                m_ctx->disconnected = false;
                m_ctx->gameAcquired = true;
                RememberGuid(m_ctx->instGUID);
                LeaveCriticalSection(&g_csDev);
                LogF("dev#%08X repaired by re-acquire (poll=%s)", m_id, HrName(hp));
                return true;
            }
        }

        // 2) full re-create of the device object
        GUID found; memset(&found, 0, sizeof(found));
        if (!FindDevice(parent, &found)) {
            m_ctx->fails++;
            LeaveCriticalSection(&g_csDev);
            if (now - m_ctx->lastFailLog > 5000) {
                m_ctx->lastFailLog = now;
                LogF("dev#%08X repair: device not present (%s) - waiting for it to come back", m_id, m_ctx->prodNameA);
            }
            return false;
        }

        Dev* nd = NULL;
        HRESULT hc = V::CreateDevice(parent, found, &nd);
        if (FAILED(hc) || !nd) {
            m_ctx->fails++;
            LeaveCriticalSection(&g_csDev);
            LogF("dev#%08X repair: CreateDevice failed (%s)", m_id, HrName(hc));
            return false;
        }

        if (m_ctx->hasFormat) nd->SetDataFormat(&m_ctx->df);
        for (size_t i = 0; i < m_ctx->props.size(); ++i)
            nd->SetProperty((REFGUID)m_ctx->props[i].guid, (LPCDIPROPHEADER)m_ctx->props[i].data.data());
        if (m_ctx->evt) nd->SetEventNotification(m_ctx->evt);
        if (m_ctx->hwnd || m_ctx->coop) nd->SetCooperativeLevel(m_ctx->hwnd, m_ctx->coop);
        HRESULT ha = nd->Acquire();
        if (FAILED(ha)) {
            LeaveCriticalSection(&g_csDev);
            LogF("dev#%08X repair: revived device acquire failed (%s)", m_id, HrName(ha));
            nd->Release();
            m_ctx->fails++;
            return false;
        }

        m_retired.push_back(real);          // keep alive: other threads may still use it
        m_ctx->instGUID = found;
        RememberGuid(found);
        m_ctx->rebinds++;
        m_ctx->disconnected = false;
        m_ctx->gameAcquired = true;
        m_real = nd;                        // pointer store is atomic on x86
        LeaveCriticalSection(&g_csDev);

        RefreshInfo();
        LogF("dev#%08X REPAIRED: re-created on %s (%s)", m_id, GuidStr(found).c_str(), m_ctx->prodNameA);
        return true;
    }
};

// ---------------------------------------------------------------------------
//  IDirectInput8 proxy
// ---------------------------------------------------------------------------
template <class V>
class TDI8 : public V::I8
{
    typedef typename V::I8    I8;
    typedef typename V::Dev   Dev;
    typedef typename V::DevCb DevCb;

public:
    explicit TDI8(I8* real) : m_real(real), m_ref(1) { if (m_real) m_real->AddRef(); }
    virtual ~TDI8() { if (m_real) m_real->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, V::Iid())) {
            AddRef(); *ppv = static_cast<I8*>(this); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID g, Dev** out, LPUNKNOWN outer) override
    {
        if (!out) return E_POINTER;
        *out = NULL;
        HRESULT hr = m_real ? m_real->CreateDevice(g, out, outer) : E_FAIL;
        if (SUCCEEDED(hr) && *out) {
            Dev* real = *out;
            TDevice<V>* w = new TDevice<V>(real, m_real, g);
            real->Release();
            *out = static_cast<Dev*>(w);
            LogF("CreateDevice %s -> ok [%s | %s]", GuidStr(g).c_str(), w->Ctx()->instNameA, w->Ctx()->prodNameA);
        } else {
            LogF("CreateDevice %s -> %s", GuidStr(g).c_str(), HrName(hr));
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, DevCb cb, LPVOID ref, DWORD flags) override
    {
        LogF("EnumDevices type=0x%08lX flags=0x%08lX", type, flags);
        return m_real ? m_real->EnumDevices(type, cb, ref, flags) : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override
    {
        HRESULT hr = m_real ? m_real->GetDeviceStatus(g) : E_FAIL;
        if (FAILED(hr) && cfg_keepAlive && IsKnownGuid(g)) {
            // The pad layer polls this once in a while; a failed status makes it
            // drop the pad for good.  We repair the device ourselves, so keep
            // reporting "attached".
            LogF("GetDeviceStatus %s -> %s, keep-alive says attached", GuidStr(g).c_str(), HrName(hr));
            return DI_OK;
        }
        if (cfg_verbose) LogF("GetDeviceStatus %s -> %s", GuidStr(g).c_str(), HrName(hr));
        return hr;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND w, DWORD f) override
    { return m_real ? m_real->RunControlPanel(w, f) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v) override
    { return m_real ? m_real->Initialize(h, v) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID g, typename V::Str name, LPGUID out) override
    { return m_real ? m_real->FindDevice(g, name, out) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(typename V::Str usr, typename V::ActMap* af, typename V::SemCb cb, LPVOID ref, DWORD flags) override
    { return m_real ? m_real->EnumDevicesBySemantics(usr, af, cb, ref, flags) : E_FAIL; }

    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK cb, typename V::CfgParams* p, DWORD f, LPVOID ref) override
    { return m_real ? m_real->ConfigureDevices(cb, p, f, ref) : E_FAIL; }

private:
    I8*  m_real;
    LONG m_ref;
};

// ---------------------------------------------------------------------------
//  watchdog thread
// ---------------------------------------------------------------------------
static void SafeTick(IWatch* w)
{
    __try { w->WatchTick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { LogF("watchdog: exception while ticking"); }
}

static DWORD WINAPI WatchThread(LPVOID)
{
    LogF("watchdog started (rebind interval %d ms)", cfg_rebindMs);
    std::vector<IWatch*> snap;
    DWORD lastBeat = GetTickCount();

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        Sleep(250);

        EnterCriticalSection(&g_csWatch);
        snap = g_watch;
        for (size_t i = 0; i < snap.size(); ++i) snap[i]->WatchAddRef();
        LeaveCriticalSection(&g_csWatch);

        for (size_t i = 0; i < snap.size(); ++i) {
            SafeTick(snap[i]);
            snap[i]->WatchRelease();
        }
        snap.clear();

        if (cfg_heartbeatSec > 0 && (GetTickCount() - lastBeat) >= (DWORD)cfg_heartbeatSec * 1000) {
            lastBeat = GetTickCount();
            EnterCriticalSection(&g_csWatch);
            snap = g_watch;
            for (size_t i = 0; i < snap.size(); ++i) snap[i]->WatchAddRef();
            LeaveCriticalSection(&g_csWatch);
            for (size_t i = 0; i < snap.size(); ++i) {
                char line[256];
                snap[i]->Stats(line, sizeof(line));
                LogF("heartbeat: %s", line);
                snap[i]->WatchRelease();
            }
            snap.clear();
        }
    }
    LogF("watchdog stopped");
    return 0;
}

// ---------------------------------------------------------------------------
//  borderless fullscreen window handling
// ---------------------------------------------------------------------------
struct WndFind
{
    DWORD pid;
    HWND  best;
    LONG  area;
};

static BOOL CALLBACK EnumWndProc(HWND h, LPARAM lp)
{
    WndFind* f = (WndFind*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER) != NULL) return TRUE;

    WCHAR cls[64] = { 0 };
    if (!GetClassNameW(h, cls, 64)) return TRUE;
    if (lstrcmpiW(cls, L"IME") == 0 || lstrcmpiW(cls, L"ConsoleWindowClass") == 0) return TRUE;

    RECT r;
    if (!GetClientRect(h, &r)) return TRUE;
    LONG a = (r.right - r.left) * (r.bottom - r.top);
    if (a > f->area) { f->area = a; f->best = h; }
    return TRUE;
}

static HWND FindGameWindow()
{    WndFind f;
    f.pid = GetCurrentProcessId();
    f.best = NULL;
    f.area = 300 * 200;          // ignore tiny helper windows
    EnumWindows(EnumWndProc, (LPARAM)&f);
    return f.best;
}

static void ApplyBorderless(HWND h)
{
    LONG style = (LONG)GetWindowLongW(h, GWL_STYLE);
    LONG border = WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

    if ((style & border) || !(style & WS_POPUP) || !(style & WS_VISIBLE)) {
        LONG want = (style & ~border) | WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN;
        SetWindowLongW(h, GWL_STYLE, want);
        LogF("borderless: style 0x%08lX -> 0x%08lX", (unsigned long)style, (unsigned long)want);
    }

    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;

    RECT cur;
    if (!GetWindowRect(h, &cur)) return;
    const RECT& t = mi.rcMonitor;
    if (cur.left != t.left || cur.top != t.top || cur.right != t.right || cur.bottom != t.bottom) {
        SetWindowPos(h, HWND_TOP, t.left, t.top, t.right - t.left, t.bottom - t.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        LogF("borderless: window rect -> %ld,%ld %ldx%ld", t.left, t.top, t.right - t.left, t.bottom - t.top);
    }
}

static DWORD WINAPI BorderThread(LPVOID)
{
    LogF("borderless: thread started (enabled=%d toggleVk=0x%02X)", cfg_borderless, cfg_toggleVk);
    bool keyDown = false;

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        Sleep(300);

        if (!g_gameWnd || !IsWindow(g_gameWnd)) { g_gameWnd = FindGameWindow(); g_wndSubclassed = FALSE; }
        if (!g_gameWnd) continue;
        if (IsIconic(g_gameWnd)) continue;

        SubclassGameWindow(g_gameWnd);

        // toggle key (only while the game owns the foreground)
        if (cfg_toggleVk) {
            bool down = (GetForegroundWindow() == g_gameWnd) &&
                        ((GetAsyncKeyState(cfg_toggleVk) & 0x8000) != 0);
            if (down && !keyDown) {
                g_borderOn = !g_borderOn;
                LogF("borderless: toggled %s", g_borderOn ? "ON" : "OFF (no longer enforcing)");
            }
            keyDown = down;
        }

        if (cfg_borderless && g_borderOn) ApplyBorderless(g_gameWnd);
    }
    LogF("borderless: thread stopped");
    return 0;
}

// ---------------------------------------------------------------------------
//  GECV (chinese patch) bootstrap
// ---------------------------------------------------------------------------
static DWORD WINAPI GecvThread(LPVOID)
{
    WCHAR path[MAX_PATH];
    swprintf_s(path, L"%s\\GECV_LOADER.dll", g_dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        LogF("GECV: GECV_LOADER.dll not found - translation patch not installed");
        return 0;
    }
    HMODULE h = LoadLibraryW(path);
    LogF("GECV: LoadLibrary -> %p (err=%lu)", (void*)h, GetLastError());
    return 0;
}

// ---------------------------------------------------------------------------
//  init helpers
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  device change notification blocking
//
//  When the pad is unplugged the engine asks IDirectInput8::GetDeviceStatus,
//  gets a failure, drops the pad from its active list and never polls it
//  again (even after the pad is back).  We repair the device ourselves, so the
//  engine must not see these notifications at all.
// ---------------------------------------------------------------------------
extern "C" HDEVNOTIFY WINAPI MyRegisterDeviceNotificationA(HANDLE, LPVOID, DWORD)
{
    LogF("blocked RegisterDeviceNotificationA");
    return (HDEVNOTIFY)(ULONG_PTR)0x1001;
}
extern "C" HDEVNOTIFY WINAPI MyRegisterDeviceNotificationW(HANDLE, LPVOID, DWORD)
{
    LogF("blocked RegisterDeviceNotificationW");
    return (HDEVNOTIFY)(ULONG_PTR)0x1001;
}
extern "C" BOOL WINAPI MyUnregisterDeviceNotification(HDEVNOTIFY)
{
    return TRUE;
}

static void PatchDeviceNotifications()
{
    BYTE* base = (BYTE*)GetModuleHandleW(NULL);
    if (!base) return;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return;

    DWORD impRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!impRva) return;

    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + impRva); imp->Name; ++imp) {
        const char* dll = (const char*)(base + imp->Name);
        if (lstrcmpiA(dll, "user32.dll") != 0) continue;
        IMAGE_THUNK_DATA32* ilt = (IMAGE_THUNK_DATA32*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + imp->FirstThunk);
        for (; ilt->u1.AddressOfData; ++ilt, ++iat) {
            if (ilt->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            const char* fn = (const char*)((IMAGE_IMPORT_BY_NAME*)(base + ilt->u1.AddressOfData))->Name;
            void* repl = NULL;
            if (lstrcmpiA(fn, "RegisterDeviceNotificationA") == 0)       repl = (void*)&MyRegisterDeviceNotificationA;
            else if (lstrcmpiA(fn, "RegisterDeviceNotificationW") == 0)  repl = (void*)&MyRegisterDeviceNotificationW;
            else if (lstrcmpiA(fn, "UnregisterDeviceNotification") == 0) repl = (void*)&MyUnregisterDeviceNotification;
            if (!repl) continue;
            DWORD old = 0;
            if (VirtualProtect(&iat->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &old)) {
                iat->u1.Function = (ULONG_PTR)repl;
                VirtualProtect(&iat->u1.Function, sizeof(ULONG_PTR), old, &old);
                ++patched;
            }
        }
    }
    LogF("device change notifications: %d import(s) blocked", patched);
}

static LRESULT CALLBACK MyWndProcW(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DEVICECHANGE && cfg_blockDevChange) {
        LogF("window: swallowed WM_DEVICECHANGE wparam=0x%lX", (unsigned long)w);
        return TRUE;
    }
    return CallWindowProcW(g_oldWndProc, h, m, w, l);
}

static LRESULT CALLBACK MyWndProcA(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DEVICECHANGE && cfg_blockDevChange) {
        LogF("window: swallowed WM_DEVICECHANGE wparam=0x%lX", (unsigned long)w);
        return TRUE;
    }
    return CallWindowProcA(g_oldWndProc, h, m, w, l);
}

static void SubclassGameWindow(HWND h)
{
    if (g_wndSubclassed || !cfg_blockDevChange) return;
    WNDPROC old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC,
        (LONG_PTR)(IsWindowUnicode(h) ? MyWndProcW : MyWndProcA));
    if (!old) return;
    g_oldWndProc = old;
    g_wndSubclassed = TRUE;
    LogF("window: subclassed %p (unicode=%d)", (void*)h, IsWindowUnicode(h) ? 1 : 0);
}

static void MakeDpiAware()
{
    // GER.exe has no dpiAware manifest entry.  Without this, Windows bitmap
    // scales the whole game window (blurry) once the window is enlarged.
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef BOOL(WINAPI* PFN_SETCTX)(HANDLE);
        PFN_SETCTX f = (PFN_SETCTX)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (f) {
            // DPI_AWARENESS_CONTEXT_SYSTEM_AWARE = -2
            if (f((HANDLE)(LONG_PTR)-2)) { LogF("dpi: system aware (context api)"); return; }
        }
        typedef BOOL(WINAPI* PFN_SETOLD)(void);
        PFN_SETOLD g = (PFN_SETOLD)GetProcAddress(u, "SetProcessDPIAware");
        if (g && g()) { LogF("dpi: system aware (legacy api)"); return; }
    }
    LogF("dpi: could not enable dpi awareness (err=%lu)", GetLastError());
}

static void LoadConfig()
{
    WCHAR ini[MAX_PATH];
    swprintf_s(ini, L"%s\\dinput8_fix.ini", g_dir);

    cfg_log          = GetPrivateProfileIntW(L"General", L"Log", 1, ini);
    cfg_keepAlive    = GetPrivateProfileIntW(L"General", L"KeepAlive", 1, ini);
    cfg_discMode     = GetPrivateProfileIntW(L"General", L"DisconnectedMode", 0, ini);
    cfg_watchdog     = GetPrivateProfileIntW(L"General", L"Watchdog", 1, ini);
    cfg_rebindMs     = GetPrivateProfileIntW(L"General", L"RebindIntervalMs", 400, ini);
    cfg_heartbeatSec = GetPrivateProfileIntW(L"General", L"HeartbeatSeconds", 5, ini);
    cfg_verbose      = GetPrivateProfileIntW(L"General", L"Verbose", 0, ini);
    if (cfg_rebindMs < 50) cfg_rebindMs = 50;

    cfg_borderless = GetPrivateProfileIntW(L"Borderless", L"Enabled", 1, ini);
    cfg_toggleVk   = GetPrivateProfileIntW(L"Borderless", L"ToggleKey", 0x7A, ini);
    cfg_dpiAware   = GetPrivateProfileIntW(L"Borderless", L"DpiAware", cfg_borderless ? 1 : 0, ini);
    cfg_diag       = GetPrivateProfileIntW(L"General", L"Diagnostics", 1, ini);
    cfg_blockDevChange = GetPrivateProfileIntW(L"General", L"BlockDeviceChange", 1, ini);
    if (cfg_toggleVk < 0 || cfg_toggleVk > 255) cfg_toggleVk = 0x7A;
    g_borderOn = (cfg_borderless != 0);
}

static void OpenLog()
{
    WCHAR p[MAX_PATH];
    swprintf_s(p, L"%s\\dinput8_fix.log", g_dir);
    g_log = _wfopen(p, L"a+");
    g_logOk = (g_log != NULL);
    if (g_logOk) {
        fseek(g_log, 0, SEEK_END);
        LogF("================================================================");
        LogF("dinput8_fix %s %s | keepAlive=%d discMode=%d watchdog=%d rebind=%dms verbose=%d",
            __DATE__, __TIME__, cfg_keepAlive, cfg_discMode, cfg_watchdog, cfg_rebindMs, cfg_verbose);
    }
}

static void LoadRealDInput()
{
    WCHAR sys[MAX_PATH], p[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf_s(p, L"%s\\dinput8.dll", sys);
    g_realDll = LoadLibraryW(p);
    if (g_realDll) g_realCreate = (PFN_DI8CREATE)GetProcAddress(g_realDll, "DirectInput8Create");
    LogF("real dinput8 -> %p, DirectInput8Create -> %p (err=%lu)",
        (void*)g_realDll, (void*)g_realCreate, GetLastError());
}

// ---------------------------------------------------------------------------
//  exports
// ---------------------------------------------------------------------------
extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!g_realCreate) return E_FAIL;

    HRESULT hr = g_realCreate(hinst, ver, riid, out, outer);
    LogF("DirectInput8Create riid=%s -> %s", GuidStr(riid).c_str(), HrName(hr));
    if (FAILED(hr) || !*out) return hr;

    if (IsEqualIID(riid, IID_IDirectInput8A)) {
        IUnknown* raw = (IUnknown*)*out;
        TDI8<VA>* w = new TDI8<VA>((IDirectInput8A*)raw);
        raw->Release();
        *out = static_cast<IDirectInput8A*>(w);
    } else if (IsEqualIID(riid, IID_IDirectInput8W)) {
        IUnknown* raw = (IUnknown*)*out;
        TDI8<VW>* w = new TDI8<VW>((IDirectInput8W*)raw);
        raw->Release();
        *out = static_cast<IDirectInput8W*>(w);
    }
    return hr;
}

extern "C" LPCDIDATAFORMAT WINAPI GetdfDIJoystickProc(void)
{
    typedef LPCDIDATAFORMAT(WINAPI* FN)(void);
    FN f = g_realDll ? (FN)GetProcAddress(g_realDll, "GetdfDIJoystick") : NULL;
    return f ? f() : NULL;
}

static FARPROC RealProc(const char* name)
{
    return g_realDll ? GetProcAddress(g_realDll, name) : NULL;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID a, REFIID b, LPVOID* c)
{
    typedef HRESULT(WINAPI* FN)(REFCLSID, REFIID, LPVOID*);
    FN f = (FN)RealProc("DllGetClassObject");
    return f ? f(a, b, c) : E_FAIL;
}

extern "C" HRESULT WINAPI DllCanUnloadNow(void)
{
    typedef HRESULT(WINAPI* FN)(void);
    FN f = (FN)RealProc("DllCanUnloadNow");
    return f ? f() : E_FAIL;
}

extern "C" HRESULT WINAPI DllRegisterServer(void)
{
    typedef HRESULT(WINAPI* FN)(void);
    FN f = (FN)RealProc("DllRegisterServer");
    return f ? f() : E_FAIL;
}

extern "C" HRESULT WINAPI DllUnregisterServer(void)
{
    typedef HRESULT(WINAPI* FN)(void);
    FN f = (FN)RealProc("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);

        GetModuleFileNameW(inst, g_dir, MAX_PATH);
        WCHAR* s = wcsrchr(g_dir, L'\\');
        if (s) *s = 0;

        InitializeCriticalSection(&g_csLog);
        InitializeCriticalSection(&g_csDev);
        InitializeCriticalSection(&g_csWatch);

        LoadConfig();
        if (cfg_log) OpenLog();
        if (cfg_diag) InitDiagnostics();
        if (cfg_dpiAware) MakeDpiAware();
        LoadRealDInput();

        if (cfg_watchdog) g_watchThread = CreateThread(NULL, 0, WatchThread, NULL, 0, NULL);
        if (cfg_blockDevChange) PatchDeviceNotifications();
        if (cfg_borderless) CreateThread(NULL, 0, BorderThread, NULL, 0, NULL);

        HANDLE t = CreateThread(NULL, 0, GecvThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_stop, 1);
        if (g_logOk && g_log) { LogF("process detach"); fclose(g_log); g_log = NULL; g_logOk = false; }
    }
    return TRUE;
}
