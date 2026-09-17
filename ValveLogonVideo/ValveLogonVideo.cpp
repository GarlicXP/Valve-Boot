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
#include <vector>
#include <algorithm>
#include <mmdeviceapi.h>
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
    return cfg;
}

// 等待音频栈就绪：仅服务运行还不够，音频端点（扬声器）也必须可用。
// 两者都就绪后 RenderFile 完整渲染（含声音）才可能成功。最多等待约 10 秒。
static void EnsureAudioReady()
{
    // 1) 等待 Audiosrv 服务运行（最多 5 秒）
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (hSCM)
    {
        SC_HANDLE hSvc = OpenServiceW(hSCM, L"Audiosrv", SERVICE_QUERY_STATUS);
        if (hSvc)
        {
            SERVICE_STATUS ss;
            for (int i = 0; i < 50; ++i)
            {
                if (QueryServiceStatus(hSvc, &ss) && ss.dwCurrentState == SERVICE_RUNNING)
                    break;
                Sleep(100);
            }
            CloseServiceHandle(hSvc);
        }
        CloseServiceHandle(hSCM);
    }

    // 2) 等待默认音频渲染端点可用（最多 10 秒）
    IMMDeviceEnumerator* pEnum = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&pEnum))))
    {
        for (int i = 0; i < 100; ++i)
        {
            IMMDevice* pDev = nullptr;
            if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)))
            {
                pDev->Release();
                break;
            }
            Sleep(100);
        }
        pEnum->Release();
    }
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

    // 等待音频栈就绪（服务 + 端点设备），避免开机早期音频未就绪导致完整渲染失败（有画面没声音）
    EnsureAudioReady();

    // 播放列表 = [内嵌视频(第一号), videos 文件夹视频...]，按配置选择本次要播的一个
    VideoConfig cfg = LoadConfig();
    std::wstring vpath;
    if (hwnd && IsWindow(hwnd))
    {
        PickedVideo pick = SelectVideo(cfg);
        vpath = pick.path.empty() ? ExtractVideoResource() : pick.path;
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
            hr = pGraph->RenderFile(vpath.c_str(), nullptr);
        }

        // 完整渲染失败：音频栈可能仍在初始化，稍等后重建并重试一次完整渲染
        if (FAILED(hr))
        {
            if (pEvent)   { pEvent->Release();   pEvent = nullptr; }
            if (pControl) { pControl->Release(); pControl = nullptr; }
            if (pGraph)   { pGraph->Release();   pGraph = nullptr; }

            Sleep(1500);

            hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                IID_IGraphBuilder, reinterpret_cast<void**>(&pGraph));
            if (SUCCEEDED(hr))
            {
                pGraph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&pControl));
                pGraph->QueryInterface(IID_IMediaEventEx, reinterpret_cast<void**>(&pEvent));
                hr = pGraph->RenderFile(vpath.c_str(), nullptr);
            }
        }

        // 重试后仍失败（音频确实不可用/编码不支持）：释放后仅渲染视频流，保证至少出画面
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
    // 窗口由 SetUsageScenario 线程统一销毁，此处不再 DestroyWindow
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
            if (!bForcePlay && IsExplorerRunning())
            {
                if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
                return S_OK;   // 锁屏/已有会话，静默跳过，不播放
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
    }
    else if (fdwReason == DLL_PROCESS_DETACH)
    {
        if (g_hVideoDone) { CloseHandle(g_hVideoDone); g_hVideoDone = nullptr; }
    }
    return TRUE;
}
