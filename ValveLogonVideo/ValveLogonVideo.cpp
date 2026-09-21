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
//       默认 25000（视频约 12.8s，预留开机时图形构建等启动开销）。仅全新开机
//       登录（explorer 尚未启动）播放；锁屏/已有会话时静默跳过，避免锁屏界面
//       先闪现造成"闪一下"。
//   播放列表 = [内嵌视频(第一号), videos 文件夹视频...]：顺序模式从内嵌开始，
//       文件夹一轮播完后回到内嵌；随机模式内嵌与文件夹视频同池随机。
//
// 设计要点：
//   1. 这是 Windows 官方唯一支持在登录界面（Winlogon 桌面）上绘制内容的方式：
//      由 LogonUI 进程加载本 DLL，运行在 Session 1 的 Winlogon 桌面上。
//   2. 本 Provider 不枚举任何 tile（GetCredentialCount 返回 0），对登录流程零干扰，
//      只在全新开机登录（CPUS_LOGON 且 explorer 未运行）时启动全屏视频播放。
//   3. 全程失败安全：任何一步失败都静默销毁窗口、不阻断登录、不弹窗。
//   4. 视频窗口使用 WS_EX_NOACTIVATE，不抢占焦点，登录密码框始终保持可用。
// ---------------------------------------------------------------

#include <windows.h>
#include <credentialprovider.h>
#include <dshow.h>
#include <strmif.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <winsvc.h>

// qedit.h 已从新版 Windows SDK 移除，手动声明 Sample Grabber 接口
// （GUID 稳定，实现/CLSID 由 strmiids.lib 提供）
EXTERN_C const IID IID_ISampleGrabberCB;
EXTERN_C const IID IID_ISampleGrabber;
EXTERN_C const CLSID CLSID_SampleGrabber;
EXTERN_C const CLSID CLSID_NullRenderer;
struct __declspec(uuid("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")) ISampleGrabberCB;
struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85")) ISampleGrabber;
MIDL_INTERFACE("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")
ISampleGrabberCB : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample* pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) = 0;
};
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabber : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};
#include <deque>
#include <vector>
#include <vector>
#include <algorithm>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <propsys.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <winsvc.h>
#include <cstdio>
#include <stdlib.h>
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

// 未公开的默认播放设备切换接口（IPolicyConfigVista，Windows 7+ 稳定）
static const CLSID CLSID_PolicyConfigClient =
{ 0x870AF99C, 0x171D, 0x4F9E, { 0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9 } };
static const IID IID_IPolicyConfigVista =
{ 0xF8679F50, 0x850A, 0x41CF, { 0x9C, 0x72, 0x43, 0x0F, 0x29, 0x02, 0x90, 0xC8 } };
struct IPolicyConfigVista : public IUnknown
{
    virtual HRESULT GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT GetDeviceFormat(PCWSTR, BOOL, WAVEFORMATEX**) = 0;
    virtual HRESULT ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT GetProcessingPeriod(PCWSTR, BOOL, PINT64, PINT64) = 0;
    virtual HRESULT SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT SetEndpointVisibility(PCWSTR, BOOL) = 0;
};

HINSTANCE g_hInst = nullptr;

static float g_volBefore = -1.f;   // 播放前主音量（-1=未快照）
static BOOL  g_muteBefore = FALSE; // 播放前静音状态
static volatile LONG g_activeThreads = 0;   // 正在播放视频的线程数（用于 DllCanUnloadNow）
static HANDLE g_hVideoDone = nullptr;       // 视频线程结束时置位，用于让 SetUsageScenario 阻塞到播完
// 诊断日志（UTF-8 追加写）：C:\ProgramData\ValveBoot\valve_play.log。
// 开机早期即使其它子系统未就绪，文件系统也可写，方便定位无声/失败位置。
static void VLog(const wchar_t* fmt, ...)
{
    wchar_t path[MAX_PATH];
    DWORD pdLen = GetEnvironmentVariableW(L"ProgramData", path, MAX_PATH);
    if (pdLen == 0 || pdLen >= MAX_PATH)
        wcscpy_s(path, L"C:\\ProgramData");
    wcscat_s(path, L"\\ValveBoot");
    CreateDirectoryW(path, nullptr);
    wcscat_s(path, L"\\valve_play.log");

    wchar_t line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(line, 1024, _TRUNCATE, fmt, ap);
    va_end(ap);

    char utf8[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (n > 0)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char out[2300];
        int len = _snprintf_s(out, _TRUNCATE, "[%02u:%02u:%02u.%03u] %s\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, utf8);
        if (len > 0)
        {
            HANDLE hFile = CreateFileW(path, FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE)
            {
                DWORD written = 0;
                WriteFile(hFile, out, (DWORD)len, &written, nullptr);
                CloseHandle(hFile);
            }
        }
    }
}

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

// 声明进程 DPI 感知：确保 GetSystemMetrics 返回物理像素而非逻辑像素。
// 高分辨率 + 高 DPI 缩放的机器（如 4K @ 200%）若不声明，窗口会按逻辑分辨率
// 创建（1920x1080），在物理屏（3840x2160）上只占左上角一小块。
static void EnsureDpiAware()
{
    static bool done = false;
    if (done) return;
    done = true;
    BOOL ok = FALSE;
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser)
    {
        typedef BOOL(WINAPI* PFN_SetProcessDpiAwarenessContext)(void*);
        PFN_SetProcessDpiAwarenessContext pfn = (PFN_SetProcessDpiAwarenessContext)
            GetProcAddress(hUser, "SetProcessDpiAwarenessContext");
        if (pfn)
            ok = pfn((void*)-4 /*DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2*/);
    }
    if (!ok)
        ok = SetProcessDPIAware();
    VLog(L"[播放] DPI 感知设置 %s", ok ? L"成功" : L"失败（沿用宿主）");
}

// 创建全屏无边框窗口（黑色背景、不抢占焦点、置顶）
static HWND CreateVideoWindow()
{
    EnsureDpiAware();
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"ValveLogonVideoClass";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClass(&wc);

    // 虚拟屏幕范围：单显示器=主屏全屏；多显示器=覆盖全部屏幕
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h  = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) { w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); vx = 0; vy = 0; }
    VLog(L"[播放] 全屏尺寸 %dx%d @(%d,%d)", w, h, vx, vy);

    // WS_EX_NOACTIVATE：窗口永不激活、不抢占键盘焦点，登录框保持可用
    return CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        L"ValveLogonVideoClass",
        L"",
        WS_POPUP,
        vx, vy, w, h,
        nullptr, nullptr, g_hInst, nullptr);
}

// 判断当前是否存在已登录的用户会话：explorer.exe 在运行 = 锁屏/已有会话。
// 开机首次登录时 explorer 尚未启动（登录成功后才由系统拉起），据此区分
// "全新开机登录"与"锁屏后的自动登录"。
static bool IsExplorerRunning()
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) { found = true; break; }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return found;
}

// 锁屏界面（LockApp.exe）是否在运行：仅真正的锁屏（Win+L）需要跳过播放。
// 相比"explorer 在运行"，LockApp 判断能正确处理：
//   - 快速启动恢复（explorer 已恢复但非锁屏）→ 播放
//   - 注销后登录（explorer 可能仍在进程列表）→ 播放
//   - Win+L 锁屏解锁 → 跳过（避免先闪一下登录界面）
static bool IsLockAppRunning()
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(hSnap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"LockApp.exe") == 0) { found = true; break; }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return found;
}

// 获取本 DLL 所在目录（视频文件夹/配置文件均相对此目录）
static std::wstring GetDllDir()
{
    WCHAR buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_hInst, buf, MAX_PATH);
    if (n == 0) return L"";
    std::wstring s(buf, n);
    size_t p = s.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L"";
    return s.substr(0, p);
}

// 播放配置（config.ini 热读取，每次开机生效，无需重编译）
struct VideoConfig
{
    int          mode;    // 0=顺序循环(默认) 1=随机
    std::wstring folder;  // 视频文件夹名（相对 DLL 目录）
    DWORD        holdMs;  // 最大阻塞毫秒
    std::wstring deviceName;  // [Audio] DeviceName：空=自动选择物理设备；关键词=锁定设备
    VideoConfig() : mode(0), folder(L"videos"), holdMs(25000) {}
};

static DWORD GetHoldMs();   // 前向声明（定义在下方）

// 手动解析 config.ini（兼容 UTF-8 BOM / ANSI / 任意编辑器保存格式）
static std::wstring IniGetString(const std::wstring& iniPath,
    const std::wstring& section, const std::wstring& key, const std::wstring& def)
{
    HANDLE hFile = CreateFileW(iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return def;
    DWORD size = GetFileSize(hFile, nullptr);
    if (size == 0 || size >= 65536) { CloseHandle(hFile); return def; }

    std::string data(size, '\0');
    DWORD read = 0;
    ReadFile(hFile, &data[0], size, &read, nullptr);
    CloseHandle(hFile);

    // 去掉 UTF-8 BOM
    if (read >= 3 && (BYTE)data[0] == 0xEF && (BYTE)data[1] == 0xBB && (BYTE)data[2] == 0xBF)
    {
        data.erase(0, 3);
        read -= 3;
    }

    // 转 UTF-16：优先 UTF-8，失败则回退系统 ANSI 代码页
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.c_str(), (int)read, nullptr, 0);
    UINT cp = (len > 0) ? CP_UTF8 : CP_ACP;
    len = MultiByteToWideChar(cp, 0, data.c_str(), (int)read, nullptr, 0);
    if (len <= 0) return def;
    std::wstring text(len, L'\0');
    MultiByteToWideChar(cp, 0, data.c_str(), (int)read, &text[0], len);

    std::wstring result = def;
    std::wstring curSec;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();

        size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos) continue;
        if (line[first] == L';') continue;   // 注释行
        if (line[first] == L'[')             // 节头
        {
            size_t close = line.find(L']', first);
            if (close != std::wstring::npos)
                curSec = line.substr(first + 1, close - first - 1);
            continue;
        }
        if (curSec != section) continue;

        size_t eq = line.find(L'=', first);
        if (eq == std::wstring::npos) continue;
        std::wstring k = line.substr(first, eq - first);
        while (!k.empty() && (k.back() == L' ' || k.back() == L'\t')) k.pop_back();
        if (_wcsicmp(k.c_str(), key.c_str()) != 0) continue;

        std::wstring v = line.substr(eq + 1);
        size_t vs = v.find_first_not_of(L" \t");
        if (vs != std::wstring::npos) v = v.substr(vs);
        size_t vc = v.find(L';');           // 行尾注释
        if (vc != std::wstring::npos) v = v.substr(0, vc);
        while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
        result = v;
        break;
    }
    return result;
}

static VideoConfig LoadConfig()
{
    VideoConfig cfg;
    std::wstring ini = GetDllDir() + L"\\config.ini";

    std::wstring mode = IniGetString(ini, L"Video", L"Mode", L"sequential");
    if (_wcsicmp(mode.c_str(), L"random") == 0)
        cfg.mode = 1;

    std::wstring folder = IniGetString(ini, L"Video", L"VideoFolder", L"videos");
    if (!folder.empty())
        cfg.folder = folder;

    // HoldMs 优先级：config.ini > 注册表 > 默认 25000
    std::wstring msStr = IniGetString(ini, L"Video", L"HoldMs", L"");
    long ms = msStr.empty() ? 0 : wcstol(msStr.c_str(), nullptr, 10);
    cfg.holdMs = (ms >= 1000 && ms <= 60000) ? (DWORD)ms : GetHoldMs();

    // [Audio] DeviceName：输出设备关键词（空=自动选择物理设备）
    cfg.deviceName = IniGetString(ini, L"Audio", L"DeviceName", L"");
    return cfg;
}

// ---- 输出设备路由（虚拟设备自动纠正）----
static bool IsVirtualName(const std::wstring& name)
{
    std::wstring lower = name;
    for (auto& c : lower) c = (wchar_t)towlower(c);
    static const wchar_t* kBad[] = { L"虚拟", L"virtual", L"steam", L"网易",
        L"netease", L"loopback", L"mix", L"wave out", L"hdmi" };
    for (const wchar_t* k : kBad)
    {
        std::wstring kl = k;
        for (auto& c : kl) c = (wchar_t)towlower(c);
        if (lower.find(kl) != std::wstring::npos)
            return true;
    }
    return false;
}

static std::wstring GetEndpointName(IMMDevice* pDev)
{
    std::wstring name = L"?";
    IPropertyStore* pProps = nullptr;
    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)))
    {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            name = pv.pwszVal;
        PropVariantClear(&pv);
        pProps->Release();
    }
    return name;
}

static std::wstring GetDefaultDeviceId()
{
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
        return L"";
    std::wstring id;
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
    {
        LPWSTR s = nullptr;
        if (SUCCEEDED(pDev->GetId(&s)) && s)
        {
            id = s;
            CoTaskMemFree(s);
        }
        pDev->Release();
    }
    pEnum->Release();
    return id;
}

// 播放前调用：若默认设备是虚拟设备（或指定了 DeviceName），切换默认播放
// 设备到目标设备，并返回是否发生了切换（播放结束后应恢复）。
static bool EnsurePhysicalDevice(const std::wstring& deviceName)
{
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
        return false;

    IMMDevice* pDef = nullptr;
    bool defOk = SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDef));
    std::wstring defName = defOk ? GetEndpointName(pDef) : L"";
    if (defOk) pDef->Release();

    std::wstring targetId, targetName;
    IMMDeviceCollection* pCol = nullptr;
    if (SUCCEEDED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol)))
    {
        UINT n = 0;
        pCol->GetCount(&n);
        for (UINT i = 0; i < n && targetId.empty(); ++i)
        {
            IMMDevice* pDev = nullptr;
            if (FAILED(pCol->Item(i, &pDev))) continue;
            std::wstring nm = GetEndpointName(pDev);
            bool isDef = defOk && (_wcsicmp(nm.c_str(), defName.c_str()) == 0);
            bool want = false;
            if (!deviceName.empty())
                want = (nm.find(deviceName) != std::wstring::npos);
            else
                want = !isDef && !IsVirtualName(nm);   // 非默认且非虚拟的物理设备
            if (want)
            {
                LPWSTR s = nullptr;
                if (SUCCEEDED(pDev->GetId(&s)) && s)
                {
                    targetId = s;
                    CoTaskMemFree(s);
                }
                targetName = nm;
            }
            pDev->Release();
        }
        pCol->Release();
    }
    pEnum->Release();

    if (targetId.empty())
    {
        if (!deviceName.empty())
            VLog(L"[音频] 未找到匹配设备：%s", deviceName.c_str());
        else
            VLog(L"[音频] 默认设备已是物理设备，无需切换");
        return false;
    }

    IPolicyConfigVista* pPolicy = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
        IID_IPolicyConfigVista, reinterpret_cast<void**>(&pPolicy));
    if (FAILED(hr) || !pPolicy)
    {
        VLog(L"[音频] 无法创建 PolicyConfig（0x%08X），无法切换设备", hr);
        return false;
    }
    HRESULT hr1 = pPolicy->SetDefaultEndpoint(targetId.c_str(), eConsole);
    HRESULT hr2 = pPolicy->SetDefaultEndpoint(targetId.c_str(), eMultimedia);
    VLog(L"[音频] 切换默认设备 -> %s（console=0x%08X，multimedia=0x%08X）",
        targetName.c_str(), hr1, hr2);
    pPolicy->Release();
    return SUCCEEDED(hr1) || SUCCEEDED(hr2);
}

static void RestoreDevice(const std::wstring& id)
{
    if (id.empty()) return;
    IPolicyConfigVista* pPolicy = nullptr;
    if (FAILED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
        IID_IPolicyConfigVista, reinterpret_cast<void**>(&pPolicy))))
        return;
    pPolicy->SetDefaultEndpoint(id.c_str(), eConsole);
    pPolicy->SetDefaultEndpoint(id.c_str(), eMultimedia);
    VLog(L"[音频] 已恢复默认设备");
    pPolicy->Release();
}

// WASAPI 主动预热：打开默认渲染端点的共享模式流并短暂运行，强制 Windows
// 音频引擎（audiodg.exe）真正完成初始化。仅"服务运行 + 端点可枚举"仍可能
// 遇到引擎未就绪，导致 DirectShow RenderFile 失败或无声；预热后音频栈必然
// 就绪。最多尝试 5 次，每次间隔 300ms。
static bool WarmUpAudio()
{
    VLog(L"[音频] WASAPI 预热开始");
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
    {
        VLog(L"[音频] 预热失败：无法创建 MMDeviceEnumerator");
        return false;
    }

    bool ok = false;
    for (int i = 0; i < 5 && !ok; ++i)
    {
        IMMDevice* pDev = nullptr;
        HRESULT hEnum = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
        if (SUCCEEDED(hEnum))
        {
            DWORD state = 0;
            pDev->GetState(&state);
            IAudioClient* pClient = nullptr;
            HRESULT hAct = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                nullptr, reinterpret_cast<void**>(&pClient));
            if (SUCCEEDED(hAct))
            {
                WAVEFORMATEX* pMix = nullptr;
                HRESULT hMix = pClient->GetMixFormat(&pMix);
                if (SUCCEEDED(hMix) && pMix)
                {
                    HRESULT hInit = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                        200000, 0, pMix, nullptr);
                    if (SUCCEEDED(hInit))
                    {
                        HRESULT hStart = pClient->Start();
                        Sleep(500);   // 让引擎完成初始化与渲染管线建立
                        pClient->Stop();
                        VLog(L"[音频] 预热成功（设备状态=%lu，Activate=0x%08X，Start=0x%08X）",
                            state, hAct, hStart);
                        ok = true;
                    }
                    else
                    {
                        VLog(L"[音频] 预热 Initialize 失败 0x%08X（第 %d 次）", hInit, i + 1);
                    }
                    CoTaskMemFree(pMix);
                }
                else
                {
                    VLog(L"[音频] 预热 GetMixFormat 失败 0x%08X", hMix);
                }
                pClient->Release();
            }
            else
            {
                VLog(L"[音频] 预热 Activate 失败 0x%08X", hAct);
            }
            pDev->Release();
        }
        else
        {
            VLog(L"[音频] 预热取默认端点失败 0x%08X（第 %d 次）", hEnum, i + 1);
        }
        if (!ok)
            Sleep(300);
    }
    pEnum->Release();
    if (!ok)
        VLog(L"[音频] 预热最终失败");
    return ok;
}

// 检查音频相关服务（含 Realtek UAD：RtkAudUService / RtkAudioService）。
// 开机早期 Realtek 用户态服务可能尚未启动，导致设备 API 正常但硬件无声；
// 此处若发现未运行则尝试启动（LogonUI 以 SYSTEM 身份运行，有权限）。
static void LogAudioServicesAndFix()
{
    static const wchar_t* kSvcs[] = { L"Audiosrv", L"AudioEndpointBuilder",
        L"RtkAudUService", L"RtkAudioService" };
    SC_HANDLE hSC = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hSC)
    {
        VLog(L"[音频] 无法打开服务管理器（错误=%u）", GetLastError());
        return;
    }
    for (const wchar_t* name : kSvcs)
    {
        SC_HANDLE hSvc = OpenServiceW(hSC, name, SERVICE_QUERY_STATUS | SERVICE_START);
        if (!hSvc)
        {
            DWORD e = GetLastError();
            if (e == ERROR_SERVICE_DOES_NOT_EXIST)
                VLog(L"[音频] 服务 %s：不存在", name);
            continue;
        }
        SERVICE_STATUS st;
        if (!QueryServiceStatus(hSvc, &st))
        {
            CloseServiceHandle(hSvc);
            continue;
        }
        if (st.dwCurrentState == SERVICE_RUNNING)
        {
            VLog(L"[音频] 服务 %s：运行中", name);
            CloseServiceHandle(hSvc);
            continue;
        }
        VLog(L"[音频] 服务 %s：状态=%u，尝试启动...", name, (UINT)st.dwCurrentState);
        if (StartServiceW(hSvc, 0, nullptr))
        {
            for (int i = 0; i < 20; ++i)   // 最多等 5 秒
            {
                Sleep(250);
                if (QueryServiceStatus(hSvc, &st) && st.dwCurrentState == SERVICE_RUNNING)
                {
                    VLog(L"[音频] 服务 %s：启动成功", name);
                    break;
                }
            }
        }
        else
            VLog(L"[音频] 服务 %s：启动失败（错误=%u）", name, GetLastError());
        CloseServiceHandle(hSvc);
    }
    CloseServiceHandle(hSC);
}

// 用 WASAPI 环回从默认渲染端点录制 ms 毫秒，统计非零样本比例。
// 返回：>=0 能量比例（0=完全无声，>0.001=有声音推送到设备）；-1=检测失败。
static float MeasureLoopbackEnergy(DWORD ms)
{
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
        return -1.f;
    IMMDevice* pDev = nullptr;
    if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
    {
        pEnum->Release();
        return -1.f;
    }
    IAudioClient* pClient = nullptr;
    HRESULT hr = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&pClient));
    if (FAILED(hr)) { pDev->Release(); pEnum->Release(); return -1.f; }

    WAVEFORMATEX* pwfx = nullptr;
    hr = pClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) { pClient->Release(); pDev->Release(); pEnum->Release(); return -1.f; }
    hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, pwfx, nullptr);
    if (FAILED(hr))
    {
        VLog(L"[音频] 环回初始化失败 0x%08X", hr);
        CoTaskMemFree(pwfx); pClient->Release(); pDev->Release(); pEnum->Release();
        return -1.f;
    }
    IAudioCaptureClient* pCapture = nullptr;
    hr = pClient->GetService(IID_PPV_ARGS(&pCapture));
    if (FAILED(hr))
    {
        VLog(L"[音频] 环回获取捕获客户端失败 0x%08X", hr);
        CoTaskMemFree(pwfx); pClient->Release(); pDev->Release(); pEnum->Release();
        return -1.f;
    }
    UINT32 frames = 0;
    pClient->GetBufferSize(&frames);
    hr = pClient->Start();
    if (FAILED(hr))
    {
        pCapture->Release(); CoTaskMemFree(pwfx); pClient->Release();
        pDev->Release(); pEnum->Release();
        return -1.f;
    }

    WORD bits = pwfx->wBitsPerSample;
    WORD ch = pwfx->nChannels ? pwfx->nChannels : 1;
    DWORD sampleBytes = bits / 8;
    UINT64 total = 0, nonZero = 0;
    DWORD end = GetTickCount() + ms;
    while ((DWORD)GetTickCount() < end)
    {
        UINT32 avail = 0;
        DWORD flags = 0;
        BYTE* data = nullptr;
        HRESULT hrG = pCapture->GetBuffer(&data, &avail, &flags, nullptr, nullptr);
        if (FAILED(hrG)) { Sleep(5); continue; }
        if (avail == 0) { pCapture->ReleaseBuffer(0); Sleep(5); continue; }
        UINT32 n = avail * ch;
        for (UINT32 i = 0; i < n; ++i)
        {
            const BYTE* p = data + (size_t)i * sampleBytes;
            bool hit = false;
            if (bits == 32)
            {
                float v; memcpy(&v, p, 4);
                hit = (v > 0.001f || v < -0.001f);
            }
            else if (bits == 16)
            {
                short v; memcpy(&v, p, 2);
                hit = (v > 16 || v < -16);
            }
            else
            {
                int v = *p;
                hit = (v != 0 && v != 255);
            }
            ++total;
            if (hit) ++nonZero;
        }
        pCapture->ReleaseBuffer(avail);
    }
    pClient->Stop();
    pCapture->Release();
    CoTaskMemFree(pwfx);
    pClient->Release(); pDev->Release(); pEnum->Release();
    float ratio = (total > 0) ? (float)((double)nonZero / (double)total) : 0.f;
    VLog(L"[音频] 环回能量检测：非零样本 %llu/%llu = %.2f%%",
        (unsigned long long)nonZero, (unsigned long long)total, ratio * 100.0);
    return ratio;
}

// 向默认渲染端点推一段非零测试音（440Hz 正弦波），用于验证音频引擎
// 是否真正输出。开机早期引擎可能 API 就绪（会话活跃/流在跑）但实际
// 输出全零数据（实测开机环回 0.00%，桌面 99.48%），推真实数据才能
// 驱动引擎进入真正工作状态。
static void PlayTone(DWORD ms)
{
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
        return;
    IMMDevice* pDev = nullptr;
    if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
    {
        pEnum->Release();
        return;
    }
    IAudioClient* pClient = nullptr;
    HRESULT hr = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&pClient));
    if (SUCCEEDED(hr)) hr = pClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
        0, 0, nullptr, nullptr);
    if (SUCCEEDED(hr))
    {
        WAVEFORMATEX* pwfx = nullptr;
        pClient->GetMixFormat(&pwfx);
        UINT32 frames = 0;
        pClient->GetBufferSize(&frames);
        IAudioRenderClient* pRender = nullptr;
        if (SUCCEEDED(pClient->GetService(IID_PPV_ARGS(&pRender))) && pwfx)
        {
            BYTE* data = nullptr;
            if (SUCCEEDED(pRender->GetBuffer(frames, &data)))
            {
                double step = 2 * 3.14159265358979323846 * 440.0 / pwfx->nSamplesPerSec;
                if (pwfx->wBitsPerSample == 32)
                {
                    float* pf = reinterpret_cast<float*>(data);
                    for (UINT32 i = 0; i < frames; ++i)
                    {
                        float v = (float)(0.15 * sin(step * i));
                        for (WORD c = 0; c < pwfx->nChannels; ++c)
                            pf[i * pwfx->nChannels + c] = v;
                    }
                }
                else if (pwfx->wBitsPerSample == 16)
                {
                    short* ps = reinterpret_cast<short*>(data);
                    for (UINT32 i = 0; i < frames; ++i)
                    {
                        short v = (short)(12000.0 * sin(step * i));
                        for (WORD c = 0; c < pwfx->nChannels; ++c)
                            ps[i * pwfx->nChannels + c] = v;
                    }
                }
                else
                {
                    int* pi = reinterpret_cast<int*>(data);
                    for (UINT32 i = 0; i < frames; ++i)
                    {
                        int v = (int)(0.15 * 2147483647.0 * sin(step * i));
                        for (WORD c = 0; c < pwfx->nChannels; ++c)
                            pi[i * pwfx->nChannels + c] = v;
                    }
                }
                pRender->ReleaseBuffer(frames, 0);
            }
            pRender->Release();
        }
        if (pwfx) CoTaskMemFree(pwfx);
        pClient->Start();
        Sleep(ms);
        pClient->Stop();
    }
    pClient->Release();
    pDev->Release();
    pEnum->Release();
}

// 推测试音 + 环回验证引擎真正输出非零音频。最多尝试 maxMs 毫秒。
static bool TestToneAndVerify(DWORD maxMs)
{
    DWORD start = GetTickCount();
    while ((DWORD)(GetTickCount() - start) < maxMs)
    {
        PlayTone(250);
        float ratio = MeasureLoopbackEnergy(200);
        if (ratio > 0.001f)
            return true;
        Sleep(300);
    }
    return false;
}

// 记录默认渲染端点的名称与 ID，并列出所有活跃渲染端点（名称/状态/是否默认），
// 用于确认 DirectShow 渲染器实际路由到哪个设备（开机早期默认设备可能变化）。
static void LogDeviceInfo()
{
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
        return;

    // 默认端点名称与 ID
    IMMDevice* pDef = nullptr;
    if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDef)))
    {
        LPWSTR defId = nullptr;
        pDef->GetId(&defId);
        IPropertyStore* pProps = nullptr;
        if (SUCCEEDED(pDef->OpenPropertyStore(STGM_READ, &pProps)))
        {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                VLog(L"[音频] 默认端点：%s", pv.pwszVal);
            PropVariantClear(&pv);
            pProps->Release();
        }
        if (defId)
        {
            VLog(L"[音频] 默认端点ID：%s", defId);
            CoTaskMemFree(defId);
        }
        pDef->Release();
    }

    // 所有活跃渲染端点列表
    IMMDeviceCollection* pCol = nullptr;
    if (SUCCEEDED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol)))
    {
        UINT n = 0;
        pCol->GetCount(&n);
        VLog(L"[音频] 活跃渲染端点共 %u 个：", n);
        for (UINT i = 0; i < n; ++i)
        {
            IMMDevice* pDev = nullptr;
            if (FAILED(pCol->Item(i, &pDev)))
                continue;
            IPropertyStore* pProps = nullptr;
            wchar_t name[128] = L"?";
            if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)))
            {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                    wcsncpy_s(name, pv.pwszVal, _TRUNCATE);
                PropVariantClear(&pv);
                pProps->Release();
            }
            VLog(L"[音频]   端点%d：%s", (int)i, name);
            pDev->Release();
        }
        pCol->Release();
    }
    pEnum->Release();
}

// 主音量检查（仅记录诊断，不修改）：桌面 18% 音量时声音正常，说明音量低
// 不是无声根因；强制改主音量还会干扰用户设置，故只记录开机早期主音量
// 与静音状态，用于判断是否音量持久化未加载（0/静音）。
static void CheckMasterVolume()
{
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
        return;
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
    {
        IAudioEndpointVolume* pVol = nullptr;
        if (SUCCEEDED(pDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
            nullptr, reinterpret_cast<void**>(&pVol))))
        {
            float level = 0;
            BOOL muted = FALSE;
            pVol->GetMasterVolumeLevelScalar(&level);
            pVol->GetMute(&muted);
            VLog(L"[音频] 主音量检查：音量=%u%%，静音=%s",
                (UINT)(level * 100), muted ? L"是" : L"否");
            pVol->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
}

// 播放前快照主音量，播放结束后恢复（保护用户音量设置）。
static void SnapshotMasterVolume()
{
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
        return;
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
    {
        IAudioEndpointVolume* pVol = nullptr;
        if (SUCCEEDED(pDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
            nullptr, reinterpret_cast<void**>(&pVol))))
        {
            pVol->GetMasterVolumeLevelScalar(&g_volBefore);
            pVol->GetMute(&g_muteBefore);
            VLog(L"[音频] 主音量快照：%u%%，静音=%s（播放后恢复）",
                (UINT)(g_volBefore * 100), g_muteBefore ? L"是" : L"否");
            pVol->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
}

static void RestoreMasterVolume()
{
    if (g_volBefore < 0.f) return;
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
        return;
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
    {
        IAudioEndpointVolume* pVol = nullptr;
        if (SUCCEEDED(pDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
            nullptr, reinterpret_cast<void**>(&pVol))))
        {
            pVol->SetMasterVolumeLevelScalar(g_volBefore, nullptr);
            pVol->SetMute(g_muteBefore, nullptr);
            float lv = 0; BOOL mu = FALSE;
            pVol->GetMasterVolumeLevelScalar(&lv);
            pVol->GetMute(&mu);
            VLog(L"[音频] 主音量已恢复：%u%%，静音=%s",
                (UINT)(lv * 100), mu ? L"是" : L"否");
            pVol->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
    g_volBefore = -1.f;
}

// ---- 音频直推引擎：Sample Grabber 抓 PCM + WASAPI 推送到所有设备 ----
// 绕开 DirectShow 的 DirectSound 渲染器（实体机开机早期它输出全零静音）。
static volatile LONG g_pushStop = 0;
static HANDLE g_pushThread = nullptr;
static HANDLE g_qEvent = nullptr;
static CRITICAL_SECTION g_qCs;
static std::deque<std::vector<BYTE>> g_q;
static volatile LONG g_pushActive = 0;
static volatile LONG g_pushExclusive = 0;   // 1=已用独占模式（环回检测不适用）

static WAVEFORMATEX g_srcFmt = {};
static bool g_srcFmtOk = false;

static void QueuePush(const BYTE* p, LONG len)
{
    if (!g_pushActive || len <= 0 || !p) return;
    EnterCriticalSection(&g_qCs);
    if (g_q.size() < 512)
        g_q.push_back(std::vector<BYTE>(p, p + len));
    LeaveCriticalSection(&g_qCs);
    if (g_qEvent) SetEvent(g_qEvent);
}

static IPin* GetPin(IBaseFilter* pFilter, PIN_DIRECTION dir)
{
    IEnumPins* pEnum = nullptr;
    if (FAILED(pFilter->EnumPins(&pEnum))) return nullptr;
    IPin* pPin = nullptr;
    while (pEnum->Next(1, &pPin, nullptr) == S_OK)
    {
        PIN_DIRECTION pd;
        if (SUCCEEDED(pPin->QueryDirection(&pd)) && pd == dir)
        {
            pEnum->Release();
            return pPin;
        }
        pPin->Release();
    }
    pEnum->Release();
    return nullptr;
}

class GrabCB : public ISampleGrabberCB
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB)
        {
            *ppv = this;
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP SampleCB(double, IMediaSample* pSample) override
    {
        if (!g_pushActive || !pSample) return S_OK;
        BYTE* p = nullptr;
        LONG len = 0;
        if (SUCCEEDED(pSample->GetPointer(&p)))
            len = pSample->GetActualDataLength();
        if (len > 0 && p)
        {
            static volatile LONG logged = 0;
            if (InterlockedExchange(&logged, 1) == 0)
                VLog(L"[直推] 首样本到达：%ld 字节", len);
            QueuePush(p, len);
        }
        return S_OK;
    }
    STDMETHODIMP BufferCB(double, BYTE*, LONG) override { return S_OK; }
};
static GrabCB g_grabCB;

static bool IsFloatFmt(const WAVEFORMATEX* w)
{
    if (!w) return false;
    if (w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const WAVEFORMATEXTENSIBLE* we =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
        return we->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

// 源 PCM 缓冲 -> 目标设备 mix format（重采样 + 声道适配 + 位深编码）
static size_t ConvertAudio(const BYTE* src, size_t srcBytes,
    const WAVEFORMATEX* srcFmt, const WAVEFORMATEX* dstFmt,
    BYTE* dst, size_t dstCap)
{
    UINT srcCh = srcFmt->nChannels ? srcFmt->nChannels : 2;
    UINT srcBits = srcFmt->wBitsPerSample ? srcFmt->wBitsPerSample : 16;
    UINT srcRate = srcFmt->nSamplesPerSec ? srcFmt->nSamplesPerSec : 48000;
    UINT dstRate = dstFmt->nSamplesPerSec ? dstFmt->nSamplesPerSec : 48000;
    UINT dstCh = dstFmt->nChannels ? dstFmt->nChannels : 2;
    UINT dstBits = dstFmt->wBitsPerSample ? dstFmt->wBitsPerSample : 32;
    size_t sampleBytes = srcBits / 8;
    if (sampleBytes == 0) return 0;
    size_t nFrames = srcBytes / (sampleBytes * srcCh);
    if (nFrames == 0) return 0;
    // 压缩格式（如 WMA tag=0x161）绝不能按 PCM 转换
    if (srcFmt->wFormatTag != WAVE_FORMAT_PCM &&
        srcFmt->wFormatTag != WAVE_FORMAT_IEEE_FLOAT &&
        srcFmt->wFormatTag != WAVE_FORMAT_EXTENSIBLE)
    {
        static volatile LONG logged = 0;
        if (InterlockedExchange(&logged, 1) == 0)
            VLog(L"[直推] 源不是 PCM（tag=%u），丢弃压缩帧", srcFmt->wFormatTag);
        return 0;
    }
    // 压缩格式（如 WMA tag=0x161）绝不能按 PCM 转换
    if (srcFmt->wFormatTag != WAVE_FORMAT_PCM &&
        srcFmt->wFormatTag != WAVE_FORMAT_IEEE_FLOAT &&
        srcFmt->wFormatTag != WAVE_FORMAT_EXTENSIBLE)
    {
        static volatile LONG logged = 0;
        if (InterlockedExchange(&logged, 1) == 0)
            VLog(L"[直推] 源不是 PCM（tag=%u），丢弃压缩帧", srcFmt->wFormatTag);
        return 0;
    }

    // 1) 源 -> float32 立体声（复用缓冲避免每样本堆分配）
    static thread_local std::vector<float> srcF;
    srcF.resize(nFrames * 2);
    for (size_t i = 0; i < nFrames; ++i)
    {
        const BYTE* p = src + i * sampleBytes * srcCh;
        float l = 0, r = 0;
        if (srcBits == 32 && IsFloatFmt(srcFmt))
        {
            float v0, v1;
            memcpy(&v0, p, 4);
            if (srcCh >= 2) { memcpy(&v1, p + 4, 4); r = v1; } else r = v0;
            l = v0;
        }
        else if (srcBits == 32)
        {
            int v0, v1;
            memcpy(&v0, p, 4);
            if (srcCh >= 2) { memcpy(&v1, p + 4, 4); r = (float)v1 / 2147483648.0f; }
            else r = (float)v0 / 2147483648.0f;
            l = (float)v0 / 2147483648.0f;
        }
        else if (srcBits == 16)
        {
            short v0, v1;
            memcpy(&v0, p, 2);
            if (srcCh >= 2) { memcpy(&v1, p + 2, 2); r = (float)v1 / 32768.0f; }
            else r = (float)v0 / 32768.0f;
            l = (float)v0 / 32768.0f;
        }
        else
        {
            l = ((int)p[0] - 128) / 128.0f;
            r = l;
        }
        srcF[i * 2] = l;
        srcF[i * 2 + 1] = r;
    }

    // 2) 重采样 + 声道适配
    double ratio = (double)dstRate / (double)srcRate;
    size_t nOut = (size_t)(nFrames * ratio) + 8;
    static thread_local std::vector<float> dstF;
    dstF.resize(nOut * dstCh);
    if (srcRate == dstRate)
    {
        nOut = nFrames;
        for (size_t i = 0; i < nOut; ++i)
        {
            dstF[i * dstCh] = srcF[i * 2];
            dstF[i * dstCh + (dstCh > 1 ? 1 : 0)] = srcF[i * 2 + 1];
            for (UINT c = 2; c < dstCh; ++c)
                dstF[i * dstCh + c] = srcF[i * 2];
        }
    }
    else
    {
        for (size_t i = 0; i < nOut; ++i)
        {
            double pos = i / ratio;
            size_t i0 = (size_t)pos;
            if (i0 >= nFrames) i0 = nFrames - 1;
            size_t i1 = (i0 + 1 < nFrames) ? i0 + 1 : i0;
            float t = (float)(pos - i0);
            float l = srcF[i0 * 2] * (1 - t) + srcF[i1 * 2] * t;
            float r = srcF[i0 * 2 + 1] * (1 - t) + srcF[i1 * 2 + 1] * t;
            dstF[i * dstCh] = l;
            dstF[i * dstCh + (dstCh > 1 ? 1 : 0)] = r;
            for (UINT c = 2; c < dstCh; ++c)
                dstF[i * dstCh + c] = l;
        }
    }

    // 3) 编码目标位深
    size_t frameBytes = dstCh * dstBits / 8;
    size_t maxOut = dstCap / frameBytes;
    if (nOut > maxOut) nOut = maxOut;
    size_t dstBytes = nOut * frameBytes;
    if (dstBits == 32 && IsFloatFmt(dstFmt))
    {
        memcpy(dst, dstF.data(), dstBytes);
    }
    else if (dstBits == 16)
    {
        short* ds = reinterpret_cast<short*>(dst);
        for (size_t i = 0; i < nOut * dstCh; ++i)
        {
            float v = dstF[i];
            if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
            ds[i] = (short)(v * 32767.0f);
        }
    }
    else
    {
        int* di = reinterpret_cast<int*>(dst);
        for (size_t i = 0; i < nOut * dstCh; ++i)
        {
            float v = dstF[i];
            if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
            di[i] = (int)(v * 2147483647.0f);
        }
    }
    return dstBytes;
}

struct PushTarget
{
    IAudioClient* client;
    IAudioRenderClient* render;
    UINT32 bufFrames;
    wchar_t name[96];
    WAVEFORMATEX* fmt;
};

// 独占模式初始化：GetMixFormat 的 32bit float 通常不被独占接受，
// 依次探测 mix 格式与 16bit PCM，成功后返回实际使用的格式
static HRESULT InitExclusive(IAudioClient* c, const WAVEFORMATEX* mix,
    WAVEFORMATEX** outFmt)
{
    if (!c || !mix) return E_INVALIDARG;
    // 候选1：mix 格式（通常是 32bit float）
    HRESULT hr = c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, mix, nullptr);
    if (hr == S_OK)
    {
        hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 150000, 0, mix, nullptr);
        if (SUCCEEDED(hr)) { *outFmt = const_cast<WAVEFORMATEX*>(mix); return hr; }
    }
    // 候选2：16bit PCM（Realtek 等物理设备几乎必支持）
    WAVEFORMATEXTENSIBLE w16;
    ZeroMemory(&w16, sizeof(w16));
    w16.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w16.Format.nChannels = mix->nChannels ? mix->nChannels : 2;
    w16.Format.nSamplesPerSec = mix->nSamplesPerSec ? mix->nSamplesPerSec : 48000;
    w16.Format.wBitsPerSample = 16;
    w16.Format.nBlockAlign = (WORD)(w16.Format.nChannels * 2);
    w16.Format.nAvgBytesPerSec = w16.Format.nSamplesPerSec * w16.Format.nBlockAlign;
    w16.Format.cbSize = 22;
    w16.Samples.wValidBitsPerSample = 16;
    w16.dwChannelMask = (w16.Format.nChannels >= 2)
        ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : SPEAKER_FRONT_CENTER;
    w16.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    hr = c->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
        reinterpret_cast<WAVEFORMATEX*>(&w16), nullptr);
    if (hr == S_OK)
    {
        hr = c->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, 150000, 0,
            reinterpret_cast<WAVEFORMATEX*>(&w16), nullptr);
        if (SUCCEEDED(hr))
        {
            WAVEFORMATEX* p = reinterpret_cast<WAVEFORMATEX*>(
                CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
            if (!p) { c->Stop(); return E_OUTOFMEMORY; }
            CopyMemory(p, &w16, sizeof(WAVEFORMATEXTENSIBLE));
            *outFmt = p;
            return hr;
        }
    }
    return E_FAIL;
}

static DWORD WINAPI PushThreadProc(LPVOID)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
    {
        CoUninitialize();
        return 1;
    }

    // 目标：所有活跃渲染端点（含默认设备）
    PushTarget targets[8];
    int targetCount = 0;
    // 默认设备优先：它是用户真正听到的设备，必须完整、优先写入
    LPWSTR defId = nullptr;
    {
        IMMDevice* pDef = nullptr;
        if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDef)))
        {
            pDef->GetId(&defId);
            pDef->Release();
        }
    }
    IMMDeviceCollection* pCol = nullptr;
    if (SUCCEEDED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol)))
    {
        UINT n = 0;
        pCol->GetCount(&n);
        for (UINT i = 0; i < n && targetCount < 8; ++i)
        {
            IMMDevice* pDev = nullptr;
            if (FAILED(pCol->Item(i, &pDev))) continue;
            IAudioClient* tClient = nullptr;
            WAVEFORMATEX* tFmt = nullptr;
            IAudioRenderClient* tRender = nullptr;
            HRESULT th = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&tClient));
            if (SUCCEEDED(th)) th = tClient->GetMixFormat(&tFmt);
            // 独占优先：未登录 SYSTEM 会话的共享流会被音频引擎丢弃（环回 0%），
            // 独占流直接写设备缓冲、绕过引擎会话路由，能真正出声
            BOOL excl = FALSE;
            if (SUCCEEDED(th))
            {
                WAVEFORMATEX* efmt = nullptr;
                th = InitExclusive(tClient, tFmt, &efmt);
                if (SUCCEEDED(th))
                {
                    excl = TRUE;
                    if (efmt != tFmt) { CoTaskMemFree(tFmt); tFmt = efmt; }
                }
            }
            if (FAILED(th))
                th = tClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                    0, 0, 0, tFmt, nullptr);
            if (SUCCEEDED(th)) th = tClient->GetService(IID_PPV_ARGS(&tRender));
            if (SUCCEEDED(th) && excl) InterlockedExchange(&g_pushExclusive, 1);
            if (SUCCEEDED(th))
            {
                PushTarget& t = targets[targetCount];
                t.client = tClient;
                t.render = tRender;
                t.fmt = tFmt;
                tClient->GetBufferSize(&t.bufFrames);
                IPropertyStore* pProps = nullptr;
                wcscpy_s(t.name, L"?");
                if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)))
                {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                        wcsncpy_s(t.name, pv.pwszVal, _TRUNCATE);
                    PropVariantClear(&pv);
                    pProps->Release();
                }
                // 默认设备交换到最前：t 是 targets[targetCount] 的引用，
                // 与 targets[0] 互换（++targetCount 之前）
                if (defId)
                {
                    LPWSTR devId = nullptr;
                    if (SUCCEEDED(pDev->GetId(&devId)) && devId)
                    {
                        if (wcscmp(devId, defId) == 0 && targetCount > 0)
                        {
                            PushTarget tmp = targets[0];
                            targets[0] = targets[targetCount];
                            targets[targetCount] = tmp;
                        }
                        CoTaskMemFree(devId);
                    }
                }
                ++targetCount;
            }
            else
            {
                if (tFmt) CoTaskMemFree(tFmt);
                if (tClient) tClient->Release();
            }
            pDev->Release();
        }
        pCol->Release();
    }
    pEnum->Release();

    if (defId) CoTaskMemFree(defId);
    if (targetCount == 0)
    {
        VLog(L"[直推] 无可用输出设备，直推未启动");
        CoUninitialize();
        return 1;
    }
    VLog(L"[直推] 启动：推送到 %d 个设备：", targetCount);
    for (int i = 0; i < targetCount; ++i)
        VLog(L"[直推]   -> %s（%uHz %uch %ubit）%s", targets[i].name,
            targets[i].fmt->nSamplesPerSec, targets[i].fmt->nChannels,
            targets[i].fmt->wBitsPerSample,
            g_pushExclusive ? L"[独占]" : L"[共享]");

    for (int i = 0; i < targetCount; ++i)
        targets[i].client->Start();

    BYTE convBuf[65536];
    UINT64 totalPushed = 0;
    while (!g_pushStop)
    {
        DWORD wait = WaitForSingleObject(g_qEvent, 200);
        std::vector<BYTE> chunk;
        EnterCriticalSection(&g_qCs);
        if (!g_q.empty())
        {
            chunk.swap(g_q.front());
            g_q.pop_front();
        }
        LeaveCriticalSection(&g_qCs);
        if (chunk.empty())
        {
            if (wait == WAIT_OBJECT_0) ResetEvent(g_qEvent);
            continue;
        }
        if (!g_srcFmtOk) continue;
        // 所有目标格式相同时只转换一次，其余设备直接复用
        bool sameFmt = true;
        for (int i = 1; i < targetCount; ++i)
        {
            if (targets[i].fmt->nSamplesPerSec != targets[0].fmt->nSamplesPerSec ||
                targets[i].fmt->nChannels != targets[0].fmt->nChannels ||
                targets[i].fmt->wBitsPerSample != targets[0].fmt->wBitsPerSample)
            { sameFmt = false; break; }
        }
        size_t cbFirst = 0;
        if (sameFmt)
            cbFirst = ConvertAudio(chunk.data(), chunk.size(), &g_srcFmt,
                targets[0].fmt, convBuf, sizeof(convBuf));
        for (int i = 0; i < targetCount; ++i)
        {
            size_t cb = cbFirst;
            if (!sameFmt)
                cb = ConvertAudio(chunk.data(), chunk.size(), &g_srcFmt,
                    targets[i].fmt, convBuf, sizeof(convBuf));
            if (cb == 0) continue;
            // 循环写：默认设备（i==0）完整写入（缓冲满时最多等 100ms），
            // 其他设备尽力写一轮，避免慢设备拖住整条音频链导致卡顿
            const BYTE* src = convBuf;
            size_t remain = cb;
            int waits = 0;
            while (remain > 0)
            {
                UINT32 pad = 0;
                if (FAILED(targets[i].client->GetCurrentPadding(&pad))) break;
                UINT32 writable = (pad < targets[i].bufFrames)
                    ? (targets[i].bufFrames - pad) : 0;
                if (writable == 0)
                {
                    if (i == 0 && waits < 100) { Sleep(1); ++waits; continue; }
                    break;   // 非默认设备或等待超限：放弃剩余
                }
                UINT32 cf = (UINT32)(remain / targets[i].fmt->nBlockAlign);
                UINT32 wf = (writable < cf) ? writable : cf;
                if (wf == 0) break;
                BYTE* tbuf = nullptr;
                if (FAILED(targets[i].render->GetBuffer(wf, &tbuf))) break;
                memcpy(tbuf, src, wf * targets[i].fmt->nBlockAlign);
                targets[i].render->ReleaseBuffer(wf, 0);
                src += wf * targets[i].fmt->nBlockAlign;
                remain -= wf * targets[i].fmt->nBlockAlign;
                totalPushed += wf;
            }
        }
    }

    VLog(L"[直推] 停止（累计推送 %llu 帧）", (unsigned long long)totalPushed);
    for (int i = 0; i < targetCount; ++i)
    {
        targets[i].client->Stop();
        targets[i].render->Release();
        targets[i].client->Release();
        CoTaskMemFree(targets[i].fmt);
    }
    CoUninitialize();
    return 0;
}

static void PushStart()
{
    // 注意：g_srcFmtOk 在建图时已确定，绝不能在此重置，否则直推线程
    // 会因 !g_srcFmtOk 永远跳过所有样本（此前 0 帧的根因）
    InitializeCriticalSection(&g_qCs);
    g_qEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    InterlockedExchange(&g_pushActive, 1);
    InterlockedExchange(&g_pushStop, 0);
    g_pushThread = CreateThread(nullptr, 0, PushThreadProc, nullptr, 0, nullptr);
}

static void PushStop()
{
    InterlockedExchange(&g_pushActive, 0);
    InterlockedExchange(&g_pushStop, 1);
    if (g_qEvent) SetEvent(g_qEvent);
    if (g_pushThread)
    {
        WaitForSingleObject(g_pushThread, 3000);
        CloseHandle(g_pushThread);
        g_pushThread = nullptr;
    }
    if (g_qEvent) { CloseHandle(g_qEvent); g_qEvent = nullptr; }
    EnterCriticalSection(&g_qCs);
    g_q.clear();
    LeaveCriticalSection(&g_qCs);
    DeleteCriticalSection(&g_qCs);
}

// ---- 全设备音频广播 ----
// 从默认渲染端点环回抓取正在播放的音频，同时转发到所有其他活跃渲染端点。
// 这样无论系统默认设备是哪个、哪个驱动在开机早期没就绪，声音都到达每一个
// 输出设备，避免"数据在流但听不到"。格式不同（采样率/位深/声道）的设备跳过。
static volatile LONG g_bcastStop = 0;
static HANDLE g_bcastThread = nullptr;

struct BcastTarget
{
    IAudioClient* client;
    IAudioRenderClient* render;
    UINT32        bufFrames;
    wchar_t       name[96];
};

static bool FmtSame(const WAVEFORMATEX* a, const WAVEFORMATEX* b)
{
    return a->wFormatTag == b->wFormatTag &&
        a->nChannels == b->nChannels &&
        a->nSamplesPerSec == b->nSamplesPerSec &&
        a->wBitsPerSample == b->wBitsPerSample;
}

static DWORD WINAPI BcastThreadProc(LPVOID)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
    {
        CoUninitialize();
        return 1;
    }

    // 源：默认渲染端点（DirectShow 输出所在）
    IMMDevice* pSrc = nullptr;
    if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pSrc)))
    {
        pEnum->Release(); CoUninitialize(); return 1;
    }
    LPWSTR srcId = nullptr;
    pSrc->GetId(&srcId);

    WAVEFORMATEX* srcFmt = nullptr;
    IAudioClient* srcClient = nullptr;
    IAudioCaptureClient* srcCap = nullptr;
    UINT32 srcFrames = 0;
    HRESULT hr = pSrc->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&srcClient));
    if (SUCCEEDED(hr)) hr = srcClient->GetMixFormat(&srcFmt);
    if (SUCCEEDED(hr)) hr = srcClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, srcFmt, nullptr);
    if (SUCCEEDED(hr)) hr = srcClient->GetService(IID_PPV_ARGS(&srcCap));
    if (SUCCEEDED(hr)) hr = srcClient->GetBufferSize(&srcFrames);
    if (FAILED(hr))
    {
        VLog(L"[广播] 环回源初始化失败 0x%08X", hr);
        if (srcCap) srcCap->Release();
        if (srcClient) srcClient->Release();
        if (srcFmt) CoTaskMemFree(srcFmt);
        if (srcId) CoTaskMemFree(srcId);
        pSrc->Release(); pEnum->Release(); CoUninitialize();
        return 1;
    }

    // 目标：所有其他活跃渲染端点（格式兼容才推）
    BcastTarget targets[8];
    int targetCount = 0;
    IMMDeviceCollection* pCol = nullptr;
    if (SUCCEEDED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol)))
    {
        UINT n = 0;
        pCol->GetCount(&n);
        for (UINT i = 0; i < n && targetCount < 8; ++i)
        {
            IMMDevice* pDev = nullptr;
            if (FAILED(pCol->Item(i, &pDev))) continue;
            LPWSTR devId = nullptr;
            pDev->GetId(&devId);
            bool isSrc = devId && srcId && wcscmp(devId, srcId) == 0;
            if (devId) CoTaskMemFree(devId);
            if (isSrc) { pDev->Release(); continue; }

            IAudioClient* tClient = nullptr;
            IAudioRenderClient* tRender = nullptr;
            WAVEFORMATEX* tFmt = nullptr;
            HRESULT th = pDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&tClient));
            if (SUCCEEDED(th)) th = tClient->GetMixFormat(&tFmt);
            if (SUCCEEDED(th) && !FmtSame(srcFmt, tFmt))
            {
                VLog(L"[广播] 跳过设备%d：格式不同（%uHz/%uch/%ubit）",
                    (int)i, tFmt ? tFmt->nSamplesPerSec : 0,
                    tFmt ? tFmt->nChannels : 0, tFmt ? tFmt->wBitsPerSample : 0);
                th = E_UNEXPECTED;
            }
            if (SUCCEEDED(th)) th = tClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                0, 0, 0, tFmt, nullptr);
            if (SUCCEEDED(th))
                th = tClient->GetService(IID_PPV_ARGS(&tRender));
            if (SUCCEEDED(th))
            {
                BcastTarget& t = targets[targetCount];
                t.client = tClient;
                t.render = tRender;
                tClient->GetBufferSize(&t.bufFrames);
                IPropertyStore* pProps = nullptr;
                wcscpy_s(t.name, L"?");
                if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)))
                {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                        wcsncpy_s(t.name, pv.pwszVal, _TRUNCATE);
                    PropVariantClear(&pv);
                    pProps->Release();
                }
                ++targetCount;
            }
            else
            {
                if (tRender) tRender->Release();
                if (tClient) tClient->Release();
            }
            if (tFmt) CoTaskMemFree(tFmt);
            pDev->Release();
        }
        pCol->Release();
    }

    if (targetCount == 0)
    {
        VLog(L"[广播] 无兼容目标设备，广播未启动");
        srcCap->Release(); srcClient->Release(); CoTaskMemFree(srcFmt);
        if (srcId) CoTaskMemFree(srcId);
        pSrc->Release(); pEnum->Release(); CoUninitialize();
        return 1;
    }
    VLog(L"[广播] 启动：转发到 %d 个设备：", targetCount);
    for (int i = 0; i < targetCount; ++i)
        VLog(L"[广播]   -> %s", targets[i].name);

    srcClient->Start();
    for (int i = 0; i < targetCount; ++i)
        targets[i].client->Start();

    while (!g_bcastStop)
    {
        UINT32 pad = 0;
        if (FAILED(srcClient->GetCurrentPadding(&pad))) break;
        UINT32 avail = srcFrames - pad;
        if (avail == 0) { Sleep(5); continue; }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        HRESULT hrG = srcCap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hrG)) { Sleep(5); continue; }
        if (frames == 0) { srcCap->ReleaseBuffer(0); Sleep(5); continue; }
        UINT32 frameBytes = frames * srcFmt->nBlockAlign;
        for (int i = 0; i < targetCount; ++i)
        {
            UINT32 tpad = 0;
            if (FAILED(targets[i].client->GetCurrentPadding(&tpad))) continue;
            UINT32 writable = targets[i].bufFrames - tpad;
            UINT32 wf = (writable < frames) ? writable : frames;
            if (wf == 0) continue;
            BYTE* tbuf = nullptr;
            if (SUCCEEDED(targets[i].render->GetBuffer(wf, &tbuf)))
            {
                memcpy(tbuf, data, wf * srcFmt->nBlockAlign);
                targets[i].render->ReleaseBuffer(wf, 0);
            }
        }
        srcCap->ReleaseBuffer(frames);
    }

    VLog(L"[广播] 停止");
    srcClient->Stop();
    for (int i = 0; i < targetCount; ++i)
    {
        targets[i].client->Stop();
        targets[i].render->Release();
        targets[i].client->Release();
    }
    srcCap->Release();
    srcClient->Release();
    CoTaskMemFree(srcFmt);
    if (srcId) CoTaskMemFree(srcId);
    pSrc->Release();
    pEnum->Release();
    CoUninitialize();
    return 0;
}

static void BcastStart()
{
    InterlockedExchange(&g_bcastStop, 0);
    g_bcastThread = CreateThread(nullptr, 0, BcastThreadProc, nullptr, 0, nullptr);
}

static void BcastStop()
{
    if (!g_bcastThread) return;
    InterlockedExchange(&g_bcastStop, 1);
    WaitForSingleObject(g_bcastThread, 3000);
    CloseHandle(g_bcastThread);
    g_bcastThread = nullptr;
}

// 强制主音量 50% + 取消静音。实测 DirectShow 渲染器在播放启动瞬间会把
// 系统主音量改成 0+静音且不恢复（导致数据在流但物理无声），必须主动对抗。
static void ForceMasterVolumeNow()
{
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
        return;
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
    {
        IAudioEndpointVolume* pMaster = nullptr;
        if (SUCCEEDED(pDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
            nullptr, reinterpret_cast<void**>(&pMaster))))
        {
            pMaster->SetMasterVolumeLevelScalar(0.5f, nullptr);   // 播放音量 50%
            pMaster->SetMute(FALSE, nullptr);
            float lv = 0; BOOL mu = FALSE;
            pMaster->GetMasterVolumeLevelScalar(&lv);
            pMaster->GetMute(&mu);
            VLog(L"[音频] 播放中主音量强制：%u%%，静音=%s",
                (UINT)(lv * 100), mu ? L"是" : L"否");
            pMaster->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
}

// 播放开始后强制本进程音频会话取消静音 + 50% 音量。
// DirectShow 渲染器创建的会话可能处于静音/低音量状态，这里主动解除。
static void ForceSessionVolume()
{
    ForceMasterVolumeNow();   // 播放启动立即强制，防止渲染器启动瞬间改 0
    // 播放刚开始时会话可能尚未激活（数据流未建立），稍等再查询真实状态
    Sleep(800);
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
        return;
    IMMDevice* pDev = nullptr;
    if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
    {
        IAudioSessionManager2* pMgr = nullptr;
        if (SUCCEEDED(pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
            nullptr, reinterpret_cast<void**>(&pMgr))))
        {
            IAudioSessionEnumerator* pSessEnum = nullptr;
            if (SUCCEEDED(pMgr->GetSessionEnumerator(&pSessEnum)))
            {
                int count = 0;
                pSessEnum->GetCount(&count);
                DWORD selfPid = GetCurrentProcessId();
                for (int i = 0; i < count; ++i)
                {
                    IAudioSessionControl* pCtl = nullptr;
                    if (FAILED(pSessEnum->GetSession(i, &pCtl)))
                        continue;
                    IAudioSessionControl2* pCtl2 = nullptr;
                    if (SUCCEEDED(pCtl->QueryInterface(IID_PPV_ARGS(&pCtl2))))
                    {
                        DWORD pid = 0;
                        if (SUCCEEDED(pCtl2->GetProcessId(&pid)) && pid == selfPid)
                        {
                            ISimpleAudioVolume* pVol = nullptr;
                            if (SUCCEEDED(pCtl->QueryInterface(__uuidof(ISimpleAudioVolume),
                                reinterpret_cast<void**>(&pVol))))
                            {
                                AudioSessionState st = AudioSessionStateInactive;
                                pCtl->GetState(&st);
                                float v = 0;
                                BOOL m = FALSE;
                                pVol->GetMasterVolume(&v);
                                pVol->GetMute(&m);
                                VLog(L"[音频] 本进程会话：状态=%d(0停 1活跃 2过期)，音量=%u%%，静音=%s（强制解除）",
                                    (int)st, (UINT)(v * 100), m ? L"是" : L"否");
                                if (!g_pushExclusive)
                                    MeasureLoopbackEnergy(800);
                                else
                                    VLog(L"[音频] 独占模式：不经过引擎混音，跳过环回检测（直推帧数即判据）");
                                pVol->SetMute(FALSE, nullptr);
                                pVol->SetMasterVolume(0.5f, nullptr);   // 播放音量 50%
                                pVol->Release();
                                ForceMasterVolumeNow();
                            }
                            pCtl2->Release();
                            pCtl->Release();
                            break;
                        }
                        pCtl2->Release();
                    }
                    pCtl->Release();
                }
                pSessEnum->Release();
            }
            pMgr->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
}

// 等待音频栈就绪：服务（Audiosrv 及其依赖 AudioEndpointBuilder）运行、
// 默认端点可枚举、且音频引擎已通过 WASAPI 预热，三者都就绪后
// RenderFile 完整渲染（含声音）才可能成功。最多等待约 12 秒。
static void EnsureAudioReady()
{
    // 1) 等待 Audiosrv 与 AudioEndpointBuilder 服务运行（各最多 5 秒）
    const wchar_t* kSvcs[] = { L"AudioEndpointBuilder", L"Audiosrv" };
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (hSCM)
    {
        for (const wchar_t* svc : kSvcs)
        {
            SC_HANDLE hSvc = OpenServiceW(hSCM, svc, SERVICE_QUERY_STATUS);
            if (hSvc)
            {
                SERVICE_STATUS ss;
                DWORD lastState = 0;
                for (int i = 0; i < 50; ++i)
                {
                    if (QueryServiceStatus(hSvc, &ss) && ss.dwCurrentState == SERVICE_RUNNING)
                        break;
                    lastState = ss.dwCurrentState;
                    Sleep(100);
                }
                QueryServiceStatus(hSvc, &ss);
                VLog(L"[音频] 服务 %s 状态=%lu", svc, ss.dwCurrentState ? ss.dwCurrentState : lastState);
                CloseServiceHandle(hSvc);
            }
            else
            {
                VLog(L"[音频] 服务 %s 无法打开（GetLastError=%lu）", svc, GetLastError());
            }
        }
        CloseServiceHandle(hSCM);
    }

    // 2) 等待默认音频渲染端点可用（最多 10 秒）
    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hEnumInit = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum));
    if (SUCCEEDED(hEnumInit))
    {
        HRESULT hLast = E_FAIL;
        for (int i = 0; i < 100; ++i)
        {
            IMMDevice* pDev = nullptr;
            hLast = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
            if (SUCCEEDED(hLast))
            {
                pDev->Release();
                break;
            }
            Sleep(100);
        }
        VLog(L"[音频] 默认端点枚举结果 0x%08X", hLast);
        pEnum->Release();
        LogDeviceInfo();
        LogAudioServicesAndFix();
    }
    else
    {
        VLog(L"[音频] MMDeviceEnumerator 创建失败 0x%08X", hEnumInit);
    }

    // 3) WASAPI 主动预热音频引擎，确保完整渲染（含声音）成功
    WarmUpAudio();

    // 4) 主音量检查：开机早期音量持久化可能未加载（0/静音），异常时强制
    CheckMasterVolume();
}

// 枚举视频文件夹中的媒体文件（按文件名排序，保证顺序稳定）
static std::vector<std::wstring> ListVideos(const std::wstring& folder)
{
    std::vector<std::wstring> files;
    static const wchar_t* kExts[] = { L"*.wmv", L"*.mp4", L"*.avi", L"*.mkv",
        L"*.mov", L"*.mpg", L"*.mpeg" };
    for (const wchar_t* ext : kExts)
    {
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW((folder + L"\\" + ext).c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                files.push_back(folder + L"\\" + fd.cFileName);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    std::sort(files.begin(), files.end(),
        [](const std::wstring& a, const std::wstring& b)
        { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return files;
}

// 一次选择的结果：path 非空 = videos 文件夹中的文件；path 为空 = 内嵌视频
struct PickedVideo
{
    std::wstring path;
};

// 顺序循环：播放列表 = [内嵌视频(第一号, index 0), 文件夹视频 1..N]。
// 传入当前索引，返回本次选择并写回下一个（每次开机只播一个；文件夹一轮播完回到内嵌）
static PickedVideo PickSequential(const std::vector<std::wstring>& files, DWORD idx)
{
    DWORD total = (DWORD)files.size() + 1;   // 内嵌恒占 index 0
    DWORD cur = idx % total;
    DWORD next = (cur + 1) % total;

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ValveBoot", 0,
        KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, L"NextVideoIndex", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&next), sizeof(next));
        RegCloseKey(hKey);
    }

    PickedVideo p;
    if (cur == 0)
        p.path = L"";                 // 内嵌视频（第一号）
    else
        p.path = files[cur - 1];       // 文件夹视频，排在内嵌之后
    return p;
}

// 随机：内嵌视频与文件夹视频同池随机（保证内嵌也会被选中）
static PickedVideo PickRandom(const std::vector<std::wstring>& files)
{
    DWORD total = (DWORD)files.size() + 1;
    DWORD cur = (DWORD)(rand() % total);
    PickedVideo p;
    if (cur == 0)
        p.path = L"";
    else
        p.path = files[cur - 1];
    return p;
}

// 按配置选择本次开机要播放的视频：列表恒含内嵌视频（第一号），
// 文件夹为空时也始终能播内嵌。
static PickedVideo SelectVideo(const VideoConfig& cfg)
{
    std::wstring dir = GetDllDir();
    std::wstring folder = (PathIsRelativeW(cfg.folder.c_str()))
        ? (dir + L"\\" + cfg.folder) : cfg.folder;
    std::vector<std::wstring> files = ListVideos(folder);
    srand(GetTickCount());

    DWORD idx = 0;
    if (cfg.mode == 0)   // 顺序模式才读索引
    {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ValveBoot", 0,
            KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            DWORD v = 0, cb = sizeof(v);
            if (RegQueryValueExW(hKey, L"NextVideoIndex", nullptr, nullptr,
                reinterpret_cast<BYTE*>(&v), &cb) == ERROR_SUCCESS)
                idx = v;
            RegCloseKey(hKey);
        }
    }

    return (cfg.mode == 1) ? PickRandom(files) : PickSequential(files, idx);
}

static DWORD WINAPI VideoThreadProc(LPVOID lpParam)
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

    // 窗口由 SetUsageScenario 同步创建并已显示（毫秒级盖住屏幕），这里只接收句柄
    HWND hwnd = static_cast<HWND>(lpParam);

    VLog(L"[播放] 视频线程启动（登录前播放）");

    // 等待音频栈就绪（服务 + 端点设备），避免开机早期音频未就绪导致完整渲染失败（有画面没声音）
    EnsureAudioReady();
    VLog(L"[播放] 音频就绪检查完成");

    // 直推方案不再需要引擎验证：音频 = Sample Grabber 抓 PCM -> WASAPI 直推，
    // 样本由本进程直接写入设备，不存在 DirectSound 引擎"假输出"问题。
    // 旧方案的测试音验证会让播放前最长等待约 14 秒（卡顿感来源），已移除。

    // 播放列表 = [内嵌视频(第一号), videos 文件夹视频...]，按配置选择本次要播的一个
    VideoConfig cfg = LoadConfig();
    std::wstring vpath;
    bool bTempVideo = false;   // true=内嵌视频释放的 Temp 副本，播完删除；源文件绝不删
    // 输出设备路由：默认设备若是虚拟设备（网易/Steam 等）则自动切换到
    // 物理扬声器，确保声音真正从扬声器出来；播放结束后恢复。
    SnapshotMasterVolume();
    std::wstring origDev = GetDefaultDeviceId();
    bool devSwitched = EnsurePhysicalDevice(cfg.deviceName);
    if (devSwitched)
    {
        Sleep(300);          // 等待设备切换生效
        WarmUpAudio();       // 对新设备预热，确保渲染到新设备
    }
    if (hwnd && IsWindow(hwnd))
    {
        PickedVideo pick = SelectVideo(cfg);
        bTempVideo = pick.path.empty();   // 内嵌视频 -> Temp 副本（可删）；文件夹源文件 -> 保留
        vpath = bTempVideo ? ExtractVideoResource() : pick.path;
        VLog(L"[播放] 选择视频：%s", vpath.c_str());
    }
    IGraphBuilder* pGraph = nullptr;
    IMediaControl* pControl = nullptr;
    IMediaEventEx* pEvent = nullptr;
    IVideoWindow* pVidWin = nullptr;
    BOOL bOk = FALSE;

    if (hwnd && !vpath.empty())
    {
        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
            IID_IGraphBuilder, reinterpret_cast<void**>(&pGraph));
        if (SUCCEEDED(hr))
        {
            pGraph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&pControl));
            pGraph->QueryInterface(IID_IMediaEventEx, reinterpret_cast<void**>(&pEvent));

            // 主路径：手动建图（视频流 -> 画面渲染；音频流 -> Sample Grabber
            // 抓 PCM，由 WASAPI 直推引擎推送到所有设备，绕开 DirectSound
            // 渲染器——实体机开机早期它输出全零静音）
            ICaptureGraphBuilder2* pBuilder = nullptr;
            HRESULT hrM = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr,
                CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2,
                reinterpret_cast<void**>(&pBuilder));
            if (SUCCEEDED(hrM))
            {
                pBuilder->SetFiltergraph(pGraph);
                IBaseFilter* pSource = nullptr;
                if (SUCCEEDED(pGraph->AddSourceFilter(vpath.c_str(), L"VideoSource", &pSource)))
                {
                    HRESULT hrV = pBuilder->RenderStream(nullptr, &MEDIATYPE_Video,
                        pSource, nullptr, nullptr);
                    VLog(L"[播放] 视频流渲染 0x%08X", hrV);
                    HRESULT hrA = E_FAIL;
                    IBaseFilter* pGrab = nullptr;
                    if (SUCCEEDED(CoCreateInstance(CLSID_SampleGrabber, nullptr,
                        CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                        reinterpret_cast<void**>(&pGrab))))
                    {
                        pGraph->AddFilter(pGrab, L"AudioGrabber");
                        ISampleGrabber* pSG = nullptr;
                        if (SUCCEEDED(pGrab->QueryInterface(IID_ISampleGrabber,
                            reinterpret_cast<void**>(&pSG))))
                        {
                            AM_MEDIA_TYPE mt;
                            ZeroMemory(&mt, sizeof(mt));
                            mt.majortype = MEDIATYPE_Audio;
                            mt.subtype = MEDIASUBTYPE_PCM;   // 只接受 PCM，逼智能连接插入 WMA 解码器
                            pSG->SetMediaType(&mt);
                            pSG->SetCallback(&g_grabCB, 0);
                            hrA = pBuilder->RenderStream(nullptr, &MEDIATYPE_Audio,
                                pSource, nullptr, pGrab);
                            VLog(L"[播放] 音频流(SampleGrabber) 渲染 0x%08X", hrA);
                            if (SUCCEEDED(hrA))
                            {
                                // Sample Grabber 是 in-place 过滤器，输出 pin 必须
                                // 有下游才会被驱动，接 Null Renderer 空渲染器
                                IBaseFilter* pNull = nullptr;
                                if (SUCCEEDED(CoCreateInstance(CLSID_NullRenderer, nullptr,
                                    CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                                    reinterpret_cast<void**>(&pNull))))
                                {
                                    pGraph->AddFilter(pNull, L"NullRenderer");
                                    IPin* pOut = GetPin(pGrab, PINDIR_OUTPUT);
                                    IPin* pIn = GetPin(pNull, PINDIR_INPUT);
                                    if (pOut && pIn)
                                    {
                                        HRESULT hrC = pGraph->Connect(pOut, pIn);
                                        VLog(L"[播放] SampleGrabber->NullRenderer 连接 0x%08X", hrC);
                                    }
                                    if (pOut) pOut->Release();
                                    if (pIn) pIn->Release();
                                    pNull->Release();
                                }
                                AM_MEDIA_TYPE cmt;
                                ZeroMemory(&cmt, sizeof(cmt));
                                if (SUCCEEDED(pSG->GetConnectedMediaType(&cmt)) &&
                                    cmt.formattype == FORMAT_WaveFormatEx && cmt.pbFormat)
                                {
                                    g_srcFmt = *reinterpret_cast<WAVEFORMATEX*>(cmt.pbFormat);
                                    g_srcFmtOk = true;
                                    VLog(L"[音频] 源格式：%uHz %uch %ubit tag=%u",
                                        g_srcFmt.nSamplesPerSec, g_srcFmt.nChannels,
                                        g_srcFmt.wBitsPerSample, g_srcFmt.wFormatTag);
                                    if (cmt.cbFormat) CoTaskMemFree(cmt.pbFormat);
                                    if (cmt.pUnk) cmt.pUnk->Release();
                                }
                            }
                            pSG->Release();
                        }
                        if (FAILED(hrA))
                            pGraph->RemoveFilter(pGrab);
                        pGrab->Release();
                    }
                    hr = SUCCEEDED(hrV) ? hrV : hrA;
                    pSource->Release();
                }
                pBuilder->Release();
            }
            else
            {
                hr = hrM;
            }
            VLog(L"[播放] 手动建图结果 HRESULT=0x%08X", hr);

            // 手动建图失败：回退 RenderFile（完整渲染，画面优先）
            if (FAILED(hr))
            {
                VLog(L"[播放] 手动建图失败，回退 RenderFile");
                if (pEvent)   { pEvent->Release();   pEvent = nullptr; }
                if (pControl) { pControl->Release(); pControl = nullptr; }
                if (pGraph)   { pGraph->Release();   pGraph = nullptr; }
                hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                    IID_IGraphBuilder, reinterpret_cast<void**>(&pGraph));
                if (SUCCEEDED(hr))
                {
                    pGraph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&pControl));
                    pGraph->QueryInterface(IID_IMediaEventEx, reinterpret_cast<void**>(&pEvent));
                    hr = pGraph->RenderFile(vpath.c_str(), nullptr);
                    VLog(L"[播放] RenderFile HRESULT=0x%08X", hr);
                }
            }
        }

        // 完整渲染失败：音频栈可能仍在初始化，每次重试前先 WASAPI 预热
        // 音频引擎，间隔递增（1.5s/3s/4.5s）重试最多 3 次，仍失败才回退
        // 仅渲染视频流（保证至少出画面）。
        for (int attempt = 1; attempt <= 3 && FAILED(hr); ++attempt)
        {
            if (pEvent)   { pEvent->Release();   pEvent = nullptr; }
            if (pControl) { pControl->Release(); pControl = nullptr; }
            if (pGraph)   { pGraph->Release();   pGraph = nullptr; }

            VLog(L"[播放] RenderFile 失败 0x%08X，预热并重试（第 %d 次）", hr, attempt);
            WarmUpAudio();                 // 强制音频引擎就绪
            Sleep(1500 * attempt);

            hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                IID_IGraphBuilder, reinterpret_cast<void**>(&pGraph));
            if (SUCCEEDED(hr))
            {
                pGraph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&pControl));
                pGraph->QueryInterface(IID_IMediaEventEx, reinterpret_cast<void**>(&pEvent));
                hr = pGraph->RenderFile(vpath.c_str(), nullptr);
                VLog(L"[播放] RenderFile(重试%d) HRESULT=0x%08X", attempt + 1, hr);
            }
        }

        // 重试后仍失败（音频确实不可用/编码不支持）：释放后分别渲染
        // 视频流与音频流——视频流保证画面，音频流尽力而为（失败不致命）
        if (FAILED(hr))
        {
            VLog(L"[播放] 完整渲染仍失败 0x%08X，回退分流渲染", hr);
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
                        HRESULT hrV = pBuilder->RenderStream(nullptr, &MEDIATYPE_Video,
                            pSource, nullptr, nullptr);
                        VLog(L"[播放] 回退：视频流渲染 0x%08X", hrV);
                        HRESULT hrA = pBuilder->RenderStream(nullptr, &MEDIATYPE_Audio,
                            pSource, nullptr, nullptr);
                        VLog(L"[播放] 回退：音频流渲染 0x%08X", hrA);
                        // 音频失败不致命：只要有视频流成功即继续播放
                        hr = SUCCEEDED(hrV) ? hrV : (SUCCEEDED(hrA) ? hrA : hrV);
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

            // 音量保险：把渲染器音量设为最大（0），防止初始静音导致无声
            IBasicAudio* pAudio = nullptr;
            if (SUCCEEDED(pGraph->QueryInterface(IID_IBasicAudio,
                reinterpret_cast<void**>(&pAudio))))
            {
                pAudio->put_Volume(0);
                VLog(L"[播放] 已设置最大音量");
                pAudio->Release();
            }

            PushStart();    // 音频直推：先启动推送线程，Run 后回调立即入队
            if (SUCCEEDED(pControl->Run()))
            {
                bOk = TRUE;
                VLog(L"[播放] 播放开始");
                ForceSessionVolume();   // 强制本进程会话取消静音 + 100%
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
                    {
                        bFinished = true;
                        VLog(L"[播放] 播放结束（事件=%ld）", ev);
                        CheckMasterVolume();   // 结束时复查主音量并记录
                    }
                }
            }
            else
            {
                VLog(L"[播放] Run() 失败 0x%08X", hr);
            }
        }
        else if (FAILED(hr))
        {
            VLog(L"[播放] 建图最终失败 0x%08X，跳过播放", hr);
        }
    }

    // ---- 清理 ----
    PushStop();    // 停止音频直推
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
    // 窗口由 SetUsageScenario 线程统一销毁，此处不再 DestroyWindow
    // 仅删除内嵌视频释放的 Temp 副本；videos 文件夹里的源文件必须保留
    if (!vpath.empty() && bTempVideo) DeleteFileW(vpath.c_str());
    if (devSwitched)
        RestoreDevice(origDev);
    RestoreMasterVolume();
    CoUninitialize();
    if (g_hVideoDone) SetEvent(g_hVideoDone);
    if (hSelf) FreeLibrary(hSelf);
    InterlockedDecrement(&g_activeThreads);
    return bOk ? 0 : 1;
}

// 读取阻塞时长（毫秒）：默认 25000，可由 HKLM\SOFTWARE\ValveBoot\LogonVideoHoldMs 覆盖
static DWORD GetHoldMs()
{
    DWORD ms = 60000;
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

// 在 SetUsageScenario 线程（窗口所属线程）上泵窗口消息，并等待视频完成。
// 窗口必须由创建它的线程泵消息；此函数同时充当"阻塞到视频播完"的 hold。
static void PumpMessagesAndWait(HWND hwnd, HANDLE hEvent, DWORD timeoutMs)
{
    if (!hEvent)
    {
        Sleep(timeoutMs);
        if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
        return;
    }

    DWORD t0 = GetTickCount();
    bool bDone = false;
    while (!bDone)
    {
        DWORD elapsed = GetTickCount() - t0;
        if (elapsed >= timeoutMs) break;

        DWORD r = MsgWaitForMultipleObjects(1, &hEvent, FALSE,
            timeoutMs - elapsed, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;    // 视频线程已结束（成功或失败）
        if (r == WAIT_TIMEOUT) break;      // 到达上限，不再等待

        // 有窗口消息（主窗口黑色背景的 WM_PAINT 等），泵掉避免窗口无响应
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { bDone = true; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
}

// ---------------------------------------------------------------------------
// Credential Provider 主体
// ---------------------------------------------------------------------------

class CValveLogonVideoProvider : public ICredentialProvider
{
public:
    CValveLogonVideoProvider() : m_cRef(1), m_bStarted(false), m_hwnd(nullptr) {}

    // 提前创建全屏窗口：CreateInstance 比 SetUsageScenario 更早被 LogonUI 调用，
    // 尽量在登录界面渲染完成前盖住屏幕。若当前无用户会话（explorer 未运行 =
    // 注销/开机登录场景）立即显示；否则保持隐藏，由 SetUsageScenario 决定。
    void Init()
    {
        if (m_hwnd) return;
        m_hwnd = CreateVideoWindow();
        if (m_hwnd && !IsExplorerRunning())
        {
            ShowWindow(m_hwnd, SW_SHOW);
            UpdateWindow(m_hwnd);
        }
    }

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
    // 在 CPUS_LOGON（正常登录）时同步创建全屏窗口并启动视频播放；其余场景一律不动作。
    STDMETHODIMP SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD /*dwFlags*/) override
    {
        if (cpus == CPUS_LOGON && !m_bStarted)
        {
            m_bStarted = true;

            // 锁屏/已有会话时不播放：锁屏界面的 LockApp 先于本 DLL 显示，
            // 无法避免"先闪一下"，因此只在全新开机登录（explorer 尚未启动）时播放。
            // 环境变量 VALVEBOOT_FORCE_PLAY=1 可强制播放（测试/调试用）。
            WCHAR wBuf[8] = { 0 };
            bool bForcePlay = GetEnvironmentVariableW(L"VALVEBOOT_FORCE_PLAY", wBuf, 8) > 0;
            if (!bForcePlay && IsLockAppRunning())
            {
                VLog(L"[播放] 锁屏场景（LockApp 运行），跳过播放");
                if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
                return S_OK;   // 仅锁屏解锁场景静默跳过，避免先闪一下登录界面
            }

            if (g_hVideoDone) ResetEvent(g_hVideoDone);

            // 窗口已在 Init() 提前创建；这里确保显示（毫秒级盖住屏幕）
            if (!m_hwnd) m_hwnd = CreateVideoWindow();
            if (m_hwnd) { ShowWindow(m_hwnd, SW_SHOW); UpdateWindow(m_hwnd); }

            HANDLE hThread = CreateThread(nullptr, 0, VideoThreadProc,
                reinterpret_cast<LPVOID>(m_hwnd), 0, nullptr);
            if (hThread)
            {
                CloseHandle(hThread);   // 分离线程，线程自行收尾
                // 在本线程（窗口所属线程）泵消息并等待视频播完（或失败/超时）。
                // LogonUI 未完成初始化前，自动登录不会触发，视频得以完整播放。
                VideoConfig cfg = LoadConfig();   // 热读取配置（修改 config.ini 下次开机生效）
                PumpMessagesAndWait(m_hwnd, g_hVideoDone, cfg.holdMs);
                m_hwnd = nullptr;   // PumpMessagesAndWait 内部已销毁窗口
            }
            else
            {
                if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
                if (g_hVideoDone) SetEvent(g_hVideoDone);   // 线程创建失败，不阻塞
            }
        }
        else
        {
            // 非登录场景（锁屏解锁等）：销毁提前创建的窗口
            if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
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
    HWND m_hwnd;
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
        p->Init();   // 提前创建并（登录场景）显示全屏窗口，尽早盖住登录界面
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
        // 诊断：记录 DLL 被哪个宿主加载（LogonUI/测试壳），定位登录前不播放
        WCHAR hostPath[MAX_PATH] = { 0 };
        GetModuleFileNameW(nullptr, hostPath, MAX_PATH);
        VLog(L"[DLL] 已加载（宿主=%s）", hostPath);
    }
    else if (fdwReason == DLL_PROCESS_DETACH)
    {
        if (g_hVideoDone) { CloseHandle(g_hVideoDone); g_hVideoDone = nullptr; }
    }
    return TRUE;
}
