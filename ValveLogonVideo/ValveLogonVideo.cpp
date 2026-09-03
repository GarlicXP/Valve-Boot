// ValveLogonVideo.cpp
//
// 登录前视频 Credential Provider
// ---------------------------------------------------------------
// 目标：Windows 启动 logo 转圈结束后、登录界面出现时，全屏播放一段内嵌视频，
//       播放结束自动消失，随后用户正常使用登录界面。
//
// 关于自动登录（无密码/免密码自动进桌面）场景：
//   LogonUI 会等所有凭据提供程序初始化完成后再放行自动登录。因此本 Provider
//   在 SetUsageScenario 中阻塞到视频播完（或超时）才返回，LogonUI 未就绪时
//   自动登录不会触发，从而保证视频完整播完，然后登录界面/桌面才出现。
//   阻塞时长可用注册表 HKLM\SOFTWARE\ValveBoot\LogonVideoHoldMs（毫秒）调节，
//       默认 25000（视频约 12.8s，预留开机时图形构建等启动开销）。此阻塞只在
//       CPUS_LOGON（开机登录）生效，锁屏解锁不受影响。
//
// 设计要点：
//   1. 这是 Windows 官方唯一支持在登录界面（Winlogon 桌面）上绘制内容的方式：
//      由 LogonUI 进程加载本 DLL，运行在 Session 1 的 Winlogon 桌面上。
//   2. 本 Provider 不枚举任何 tile（GetCredentialCount 返回 0），对登录流程零干扰，
//      只在 CPUS_LOGON 场景下异步启动一个全屏视频播放线程。
//   3. 全程失败安全：任何一步失败都静默销毁窗口、不阻断登录、不弹窗。
//   4. 视频窗口使用 WS_EX_NOACTIVATE，不抢占焦点，登录密码框始终保持可用。
// ---------------------------------------------------------------

#include <windows.h>
#include <credentialprovider.h>
#include <dshow.h>
#include <strmif.h>
#include <shlwapi.h>
#include <string>
#include <cwchar>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uuid.lib")

// 内嵌视频资源 ID（需与 ValveLogonVideo.rc 一致）
#define IDR_VIDEO_RCDATA 130

// Credential Provider 的 CLSID
static const CLSID CLSID_ValveLogonVideo =
{ 0x98deb984, 0x1c7e, 0x4531, { 0xb5, 0xd2, 0x9d, 0x32, 0x96, 0xb4, 0x16, 0xf9 } };

HINSTANCE g_hInst = nullptr;
static volatile LONG g_activeThreads = 0;   // 正在播放视频的线程数（用于 DllCanUnloadNow）
static HANDLE g_hVideoDone = nullptr;       // 视频线程结束时置位，用于让 SetUsageScenario 阻塞到播完

// 前向声明（VideoThreadProc 中用于钉住本 DLL 模块）
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved);

// ---------------------------------------------------------------------------
// 视频播放线程：在独立的 STA 线程上建窗 + DirectShow 播放，避免阻塞 LogonUI
// ---------------------------------------------------------------------------

// 从本 DLL 资源提取视频到临时文件
static std::wstring ExtractVideoResource()
{
    HRSRC hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(IDR_VIDEO_RCDATA), RT_RCDATA);
    if (!hRes) return L"";
    HGLOBAL hData = LoadResource(g_hInst, hRes);
    if (!hData) return L"";
    void* pData = LockResource(hData);
    DWORD size = SizeofResource(g_hInst, hRes);
    if (!pData || size == 0) return L"";

    WCHAR tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring file = std::wstring(tmp) + L"ValveBoot_logon.wmv";

    HANDLE hFile = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return L"";

    DWORD written = 0;
    WriteFile(hFile, pData, size, &written, nullptr);
    CloseHandle(hFile);

    if (written != size)
    {
        DeleteFileW(file.c_str());
        return L"";
    }
    return file;
}

// 创建全屏无边框窗口（黑色背景、不抢占焦点、置顶）
static HWND CreateVideoWindow()
{
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"ValveLogonVideoClass";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClass(&wc);

    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);

    // WS_EX_NOACTIVATE：窗口永不激活、不抢占键盘焦点，登录框保持可用
    return CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        L"ValveLogonVideoClass",
        L"",
        WS_POPUP,
        0, 0, w, h,
        nullptr, nullptr, g_hInst, nullptr);
}

static DWORD WINAPI VideoThreadProc(LPVOID /*lpParam*/)
{
    InterlockedIncrement(&g_activeThreads);

    // 钉住本 DLL：即使 LogonUI 在视频结束前卸载 Provider，代码页也保持有效
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&DllMain), &hSelf);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        if (g_hVideoDone) SetEvent(g_hVideoDone);
        if (hSelf) FreeLibrary(hSelf);
        InterlockedDecrement(&g_activeThreads);
        return 1;
    }

    HWND hwnd = CreateVideoWindow();
    std::wstring vpath;
    IGraphBuilder* pGraph = nullptr;
    IMediaControl* pControl = nullptr;
    IMediaEventEx* pEvent = nullptr;
    IVideoWindow* pVidWin = nullptr;
    BOOL bOk = FALSE;

    if (hwnd)
    {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        vpath = ExtractVideoResource();
    }

    if (hwnd && !vpath.empty())
    {
        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
            IID_IGraphBuilder, reinterpret_cast<void**>(&pGraph));
        if (SUCCEEDED(hr))
        {
            pGraph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&pControl));
            pGraph->QueryInterface(IID_IMediaEventEx, reinterpret_cast<void**>(&pEvent));
            hr = pGraph->RenderFile(vpath.c_str(), nullptr);
        }

        // 完整渲染失败（例如登录早期音频设备未就绪）：释放后仅渲染视频流
        if (FAILED(hr))
        {
            if (pEvent)   { pEvent->Release();   pEvent = nullptr; }
            if (pControl) { pControl->Release(); pControl = nullptr; }
            if (pGraph)   { pGraph->Release();   pGraph = nullptr; }

            hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                IID_IGraphBuilder, reinterpret_cast<void**>(&pGraph));
            if (SUCCEEDED(hr))
            {
                pGraph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&pControl));
                pGraph->QueryInterface(IID_IMediaEventEx, reinterpret_cast<void**>(&pEvent));

                ICaptureGraphBuilder2* pBuilder = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr,
                    CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2,
                    reinterpret_cast<void**>(&pBuilder))))
                {
                    pBuilder->SetFiltergraph(pGraph);
                    IBaseFilter* pSource = nullptr;
                    hr = pGraph->AddSourceFilter(vpath.c_str(), L"VideoSource", &pSource);
                    if (SUCCEEDED(hr))
                    {
                        hr = pBuilder->RenderStream(nullptr, &MEDIATYPE_Video,
                            pSource, nullptr, nullptr);
                        pSource->Release();
                    }
                    pBuilder->Release();
                }
            }
        }

        // 关键：IVideoWindow 必须建图成功后查询
        if (SUCCEEDED(hr) && !pVidWin)
            pGraph->QueryInterface(IID_IVideoWindow, reinterpret_cast<void**>(&pVidWin));

        if (SUCCEEDED(hr) && pControl && pVidWin)
        {
            pVidWin->put_Owner(reinterpret_cast<OAHWND>(hwnd));
            pVidWin->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS);
            RECT rc;
            GetClientRect(hwnd, &rc);
            pVidWin->SetWindowPosition(0, 0, rc.right, rc.bottom);

            if (SUCCEEDED(pControl->Run()))
            {
                bOk = TRUE;
                // 消息循环，直到视频播放完成
                bool bFinished = false;
                MSG msg;
                while (!bFinished)
                {
                    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
                    {
                        if (msg.message == WM_QUIT) { bFinished = true; break; }
                        TranslateMessage(&msg);
                        DispatchMessage(&msg);
                    }
                    long ev = 0;
                    if (pEvent && pEvent->WaitForCompletion(50, &ev) == S_OK &&
                        (ev == EC_COMPLETE || ev == EC_USERABORT))
                        bFinished = true;
                }
            }
        }
    }

    // ---- 清理 ----
    if (pControl) pControl->Stop();
    if (pVidWin)
    {
        pVidWin->put_Visible(OAFALSE);
        pVidWin->put_Owner(NULL);
        pVidWin->Release();
    }
    if (pEvent)   pEvent->Release();
    if (pControl) pControl->Release();
    if (pGraph)   pGraph->Release();
    if (hwnd)     DestroyWindow(hwnd);
    if (!vpath.empty()) DeleteFileW(vpath.c_str());
    CoUninitialize();
    if (g_hVideoDone) SetEvent(g_hVideoDone);
    if (hSelf) FreeLibrary(hSelf);
    InterlockedDecrement(&g_activeThreads);
    return bOk ? 0 : 1;
}

// 读取阻塞时长（毫秒）：默认 25000，可由 HKLM\SOFTWARE\ValveBoot\LogonVideoHoldMs 覆盖
static DWORD GetHoldMs()
{
    DWORD ms = 25000;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ValveBoot", 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD v = 0, cb = sizeof(v);
        if (RegQueryValueExW(hKey, L"LogonVideoHoldMs", nullptr, nullptr,
            reinterpret_cast<BYTE*>(&v), &cb) == ERROR_SUCCESS && v >= 1000 && v <= 60000)
            ms = v;
        RegCloseKey(hKey);
    }
    return ms;
}

// ---------------------------------------------------------------------------
// Credential Provider 主体
// ---------------------------------------------------------------------------

class CValveLogonVideoProvider : public ICredentialProvider
{
public:
    CValveLogonVideoProvider() : m_cRef(1), m_bStarted(false) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_ICredentialProvider)
        {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = InterlockedDecrement(&m_cRef);
        if (r == 0) delete this;
        return r;
    }

    // ICredentialProvider
    // 在 CPUS_LOGON（正常登录）时异步启动视频播放；其余场景一律不动作。
    STDMETHODIMP SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD /*dwFlags*/) override
    {
        if (cpus == CPUS_LOGON && !m_bStarted)
        {
            m_bStarted = true;
            if (g_hVideoDone) ResetEvent(g_hVideoDone);
            HANDLE hThread = CreateThread(nullptr, 0, VideoThreadProc, nullptr, 0, nullptr);
            if (hThread)
            {
                CloseHandle(hThread);   // 分离线程，线程自行收尾
                // 阻塞到视频播完（或失败/超时）。LogonUI 未完成初始化前，
                // 自动登录不会触发，视频得以完整播放。
                if (g_hVideoDone) WaitForSingleObject(g_hVideoDone, GetHoldMs());
            }
            else if (g_hVideoDone) SetEvent(g_hVideoDone);   // 线程创建失败，不阻塞
        }
        return S_OK;   // 始终 S_OK，绝不阻断 LogonUI
    }

    STDMETHODIMP SetSerialization(const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION*) override { return E_NOTIMPL; }

    STDMETHODIMP Advise(ICredentialProviderEvents*, UINT_PTR) override { return S_OK; }
    STDMETHODIMP UnAdvise() override { return S_OK; }

    // 不提供任何 tile：对登录界面零干扰
    STDMETHODIMP GetCredentialCount(DWORD* pdwCount, DWORD* pdwDefault,
        BOOL* pbAutoLogonWithDefault) override
    {
        if (pdwCount)               *pdwCount = 0;
        if (pdwDefault)             *pdwDefault = 0;
        if (pbAutoLogonWithDefault) *pbAutoLogonWithDefault = FALSE;
        return S_OK;
    }

    STDMETHODIMP GetFieldDescriptorCount(DWORD* pdwCount) override
    {
        if (pdwCount) *pdwCount = 0;
        return S_OK;
    }
    STDMETHODIMP GetFieldDescriptorAt(DWORD, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR**) override { return E_INVALIDARG; }
    STDMETHODIMP GetCredentialAt(DWORD, ICredentialProviderCredential**) override { return E_INVALIDARG; }

private:
    LONG m_cRef;
    volatile bool m_bStarted;
};

// ---------------------------------------------------------------------------
// 类工厂
// ---------------------------------------------------------------------------

class CValveClassFactory : public IClassFactory
{
public:
    CValveClassFactory() : m_cRef(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_cRef); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = InterlockedDecrement(&m_cRef);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override
    {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        CValveLogonVideoProvider* p = new CValveLogonVideoProvider();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL) override { return S_OK; }

private:
    LONG m_cRef;
};

// ---------------------------------------------------------------------------
// DLL 导出
// ---------------------------------------------------------------------------

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (rclsid != CLSID_ValveLogonVideo) return CLASS_E_CLASSNOTAVAILABLE;
    CValveClassFactory* f = new CValveClassFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return (g_activeThreads == 0) ? S_OK : S_FALSE;
}

// 注册表写入辅助
static LONG RegWriteString(HKEY hRoot, const std::wstring& subKey,
    const std::wstring& valueName, const std::wstring& value)
{
    HKEY hKey = nullptr;
    LONG lr = RegCreateKeyExW(hRoot, subKey.c_str(), 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (lr != ERROR_SUCCESS) return lr;
    lr = RegSetValueExW(hKey, valueName.empty() ? nullptr : valueName.c_str(),
        0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return lr;
}

// 递归删除注册表键及其子键
static LONG RegDeleteTreeKey(HKEY hRoot, const std::wstring& subKey)
{
    HKEY hKey = nullptr;
    LONG lr = RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_READ | KEY_WRITE, &hKey);
    if (lr != ERROR_SUCCESS) return lr;

    WCHAR subName[MAX_PATH];
    while (RegEnumKeyW(hKey, 0, subName, MAX_PATH) == ERROR_SUCCESS)
    {
        std::wstring child = subKey + L"\\" + subName;
        RegDeleteTreeKey(hRoot, child);
    }
    RegCloseKey(hKey);
    return RegDeleteKeyW(hRoot, subKey.c_str());
}

STDAPI DllRegisterServer()
{
    WCHAR dllPath[MAX_PATH];
    GetModuleFileNameW(g_hInst, dllPath, MAX_PATH);

    WCHAR clsidStr[64] = { 0 };
    StringFromGUID2(CLSID_ValveLogonVideo, clsidStr, 64);

    std::wstring keyClsid  = std::wstring(L"CLSID\\") + clsidStr;
    std::wstring keyInproc = keyClsid + L"\\InprocServer32";
    std::wstring keyCP = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
        L"Authentication\\Credential Providers\\") + clsidStr;

    // 1) 标准 COM 注册
    RegWriteString(HKEY_CLASSES_ROOT, keyClsid,  L"", L"Valve Boot Logon Video");
    RegWriteString(HKEY_CLASSES_ROOT, keyInproc, L"", dllPath);
    RegWriteString(HKEY_CLASSES_ROOT, keyInproc, L"ThreadingModel", L"Apartment");

    // 2) 注册为凭据提供程序（LogonUI 据此枚举）
    RegWriteString(HKEY_LOCAL_MACHINE, keyCP, L"", L"Valve Boot Logon Video");

    return S_OK;
}

STDAPI DllUnregisterServer()
{
    WCHAR clsidStr[64] = { 0 };
    StringFromGUID2(CLSID_ValveLogonVideo, clsidStr, 64);

    std::wstring keyCP = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
        L"Authentication\\Credential Providers\\") + clsidStr;
    std::wstring keyClsid = std::wstring(L"CLSID\\") + clsidStr;

    RegDeleteTreeKey(HKEY_LOCAL_MACHINE, keyCP);
    RegDeleteTreeKey(HKEY_CLASSES_ROOT, keyClsid);
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        g_hInst = hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        // 手动重置、初始置位：若从未重置则永不阻塞
        g_hVideoDone = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    }
    else if (fdwReason == DLL_PROCESS_DETACH)
    {
        if (g_hVideoDone) { CloseHandle(g_hVideoDone); g_hVideoDone = nullptr; }
    }
    return TRUE;
}
