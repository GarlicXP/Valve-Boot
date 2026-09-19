// Valve Boot.cpp : 开机登录前视频的安装/卸载器。
// 视频内嵌于 exe 用于本地预览；真正的登录前播放由 ValveLogonVideo.dll
// （Credential Provider）在登录界面（Winlogon 桌面）完成。
// 安装时：注册 DLL、关闭系统锁屏界面、创建 videos 文件夹与 config.ini；
// 登录前播放支持多视频顺序/随机轮播（详见 config.ini 与使用说明）。
//

#include <windows.h>
#include <dshow.h>
#include <string>
#include <cwchar>
#include <shlwapi.h>
#include <mmsystem.h>
#include <winreg.h>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winmm.lib")

// 资源 ID：必须与 Valve Boot.rc 中内嵌视频资源的实际 ID 一致。
// .rc 里视频注册为 "IDR_RCDATA1  RCDATA  \"rcdata1.bin\""，IDR_RCDATA1 = 130。
// 原来写成 101（IDR_VIDEO_STARTUP），与 .rc 不一致，导致 FindResource 找不到内嵌视频，
// 只能退回去读 exe 旁边的 startup.wmv —— 这就是“视频内嵌”失效的根因。
#define IDR_VIDEO_STARTUP 130

// 全局变量
IGraphBuilder* g_pGraph = nullptr;
IMediaControl* g_pControl = nullptr;
IMediaEventEx* g_pEvent = nullptr;
IVideoWindow* g_pVidWin = nullptr;
HWND             g_hwnd = nullptr;

// 窗口过程
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT)
        {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// 创建全屏无边框窗口（隐藏鼠标指针）
HWND CreateFullscreenWindow(HINSTANCE hInstance)
{
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ValveBootPlayerClass";
    wc.hCursor = NULL;                  // 窗口内不显示鼠标
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClass(&wc);

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    return CreateWindowEx(
        WS_EX_TOPMOST,
        L"ValveBootPlayerClass",
        L"",
        WS_POPUP,
        0, 0,
        screenWidth, screenHeight,
        nullptr, nullptr, hInstance, nullptr);
}

// 获取 exe 所在目录
std::wstring GetExeDirectory()
{
    WCHAR exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring fullPath = exePath;
    size_t pos = fullPath.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        fullPath = fullPath.substr(0, pos);
    return fullPath;
}

// 检查管理员权限
bool IsRunAsAdmin()
{
    BOOL isElevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, size, &size))
            isElevated = elevation.TokenIsElevated;
        CloseHandle(hToken);
    }
    return isElevated != FALSE;
}

// 执行 schtasks 命令
bool RunSchtasksCommand(const std::wstring& arguments)
{
    std::wstring command = L"schtasks " + arguments;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode == 0;
}

// 调用本机 DLL 的 DllRegisterServer / DllUnregisterServer，注册/注销登录前视频（Credential Provider）
static bool CallDllServerFunc(const std::wstring& dllPath, const char* funcName)
{
    HMODULE hDll = LoadLibraryW(dllPath.c_str());
    if (!hDll) return false;
    typedef HRESULT(STDAPICALLTYPE* PfnServer)();
    PfnServer pfn = (PfnServer)GetProcAddress(hDll, funcName);
    if (!pfn) { FreeLibrary(hDll); return false; }
    HRESULT hr = pfn();
    FreeLibrary(hDll);
    return SUCCEEDED(hr);
}

// 注册登录前视频（Credential Provider）
static bool RegisterLogonProvider(const std::wstring& dllPath)
{
    return CallDllServerFunc(dllPath, "DllRegisterServer");
}

// 注销登录前视频（Credential Provider）
static bool UnregisterLogonProvider(const std::wstring& dllPath)
{
    return CallDllServerFunc(dllPath, "DllUnregisterServer");
}

// 删除任务
void DeleteStartupTask()
{
    std::wstring taskName = L"ValveBootStartupVideo";
    std::wstring args = L"/delete /tn \"" + taskName + L"\" /f";
    RunSchtasksCommand(args);
}

// 注册视频同步器开机自启（HKLM Run，每次登录后启动，后台检查 GitHub 更新）
void AddUpdateAutostart(const std::wstring& exeDir)
{
    std::wstring updater = exeDir + L"\\ValveVideoUpdater.exe";
    if (GetFileAttributesW(updater.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;   // 没有同步器文件则不注册
    std::wstring cmd = L"\"" + updater + L"\"";
    RegSetKeyValueW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"ValveVideoUpdater", REG_SZ, cmd.c_str(),
        (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
}

// 移除同步器开机自启（卸载/取消时调用）
void RemoveUpdateAutostart()
{
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"ValveVideoUpdater");
    // 结束正在运行的后台同步器
    system("taskkill /IM ValveVideoUpdater.exe /F >nul 2>&1");
}

// 启动音频服务并等待就绪
void EnsureAudioServiceRunning()
{
    // 启动 Audiosrv 服务
    system("net start audiosrv >nul 2>&1");
    // 等待最多 5 秒让音频设备就绪
    for (int i = 0; i < 50; ++i)
    {
        SC_HANDLE hSCManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (hSCManager)
        {
            SC_HANDLE hService = OpenServiceW(hSCManager, L"Audiosrv", SERVICE_QUERY_STATUS);
            if (hService)
            {
                SERVICE_STATUS status;
                if (QueryServiceStatus(hService, &status) && status.dwCurrentState == SERVICE_RUNNING)
                {
                    CloseServiceHandle(hService);
                    CloseServiceHandle(hSCManager);
                    return;
                }
                CloseServiceHandle(hService);
            }
            CloseServiceHandle(hSCManager);
        }
        Sleep(100);
    }
}

// 从资源提取视频到临时文件
std::wstring ExtractVideoResource()
{
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_VIDEO_STARTUP), RT_RCDATA);
    if (!hRes) return L"";
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) return L"";
    void* pData = LockResource(hData);
    DWORD dataSize = SizeofResource(nullptr, hRes);
    if (!pData || dataSize == 0) return L"";

    WCHAR tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring tempFile = std::wstring(tempPath) + L"ValveBoot_startup.wmv";

    HANDLE hFile = CreateFileW(tempFile.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return L"";

    DWORD written = 0;
    WriteFile(hFile, pData, dataSize, &written, nullptr);
    CloseHandle(hFile);

    if (written != dataSize)
    {
        DeleteFileW(tempFile.c_str());
        return L"";
    }
    return tempFile;
}

// 播放视频（带声音，容错处理）
int RunPlaybackMode(const std::wstring& videoPath)
{
    // 初始化 COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"COM 初始化失败", L"Valve Boot", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 启动音频服务（尽力而为）
    EnsureAudioServiceRunning();

    // 创建全屏窗口（隐藏鼠标）
    g_hwnd = CreateFullscreenWindow(GetModuleHandleW(nullptr));
    if (!g_hwnd)
    {
        CoUninitialize();
        return 1;
    }
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    // 创建 Filter Graph Manager
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
        IID_IGraphBuilder, (void**)&g_pGraph);
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"无法创建 Filter Graph", L"Valve Boot", MB_OK | MB_ICONERROR);
        DestroyWindow(g_hwnd);
        CoUninitialize();
        return 1;
    }

    // 查询接口
    // 注意：IVideoWindow 由图内的视频渲染器提供，必须在 RenderFile 建图成功之后再查询；
    // 在建图前查询只会得到 NULL，导致视频画面没有挂到窗口上（表现为“只有声音没有画面”）。
    g_pGraph->QueryInterface(IID_IMediaControl, (void**)&g_pControl);
    g_pGraph->QueryInterface(IID_IMediaEventEx, (void**)&g_pEvent);

    // 尝试渲染文件（完整音视频）
    hr = g_pGraph->RenderFile(videoPath.c_str(), nullptr);
    if (FAILED(hr))
    {
        // 完整渲染失败（常见于开机阶段音频设备尚未就绪），释放后仅渲染视频流
        if (g_pEvent) { g_pEvent->Release();  g_pEvent = nullptr; }
        if (g_pControl) { g_pControl->Release();g_pControl = nullptr; }
        if (g_pGraph) { g_pGraph->Release();  g_pGraph = nullptr; }

        // 重新创建 Graph
        hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
            IID_IGraphBuilder, (void**)&g_pGraph);
        if (SUCCEEDED(hr))
        {
            g_pGraph->QueryInterface(IID_IMediaControl, (void**)&g_pControl);
            g_pGraph->QueryInterface(IID_IMediaEventEx, (void**)&g_pEvent);

            // 仅渲染视频流（跳过音频，避免音频设备不可用时整个失败）
            ICaptureGraphBuilder2* pBuilder = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr,
                CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2, (void**)&pBuilder)))
            {
                pBuilder->SetFiltergraph(g_pGraph);
                IBaseFilter* pSource = nullptr;
                hr = g_pGraph->AddSourceFilter(videoPath.c_str(), L"VideoSource", &pSource);
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

    // 建图成功后再从图中查询视频窗口接口（关键修复：之前在建图前查询得到 NULL）
    if (SUCCEEDED(hr) && !g_pVidWin)
        g_pGraph->QueryInterface(IID_IVideoWindow, (void**)&g_pVidWin);

    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"无法播放该视频文件，请确认格式为 WMV", L"Valve Boot", MB_OK | MB_ICONERROR);
        if (g_pVidWin) { g_pVidWin->Release(); g_pVidWin = nullptr; }
        if (g_pEvent) { g_pEvent->Release();  g_pEvent = nullptr; }
        if (g_pControl) { g_pControl->Release();g_pControl = nullptr; }
        if (g_pGraph) { g_pGraph->Release();  g_pGraph = nullptr; }
        DestroyWindow(g_hwnd);
        CoUninitialize();
        return 1;
    }

    // 设置视频输出窗口
    if (g_pVidWin)
    {
        g_pVidWin->put_Owner((OAHWND)g_hwnd);
        g_pVidWin->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS);
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        g_pVidWin->SetWindowPosition(0, 0, rc.right, rc.bottom);
    }

    // 开始播放
    hr = g_pControl->Run();
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, L"无法开始播放", L"Valve Boot", MB_OK | MB_ICONERROR);
        if (g_pVidWin) { g_pVidWin->Release(); g_pVidWin = nullptr; }
        if (g_pEvent) { g_pEvent->Release();  g_pEvent = nullptr; }
        if (g_pControl) { g_pControl->Release();g_pControl = nullptr; }
        if (g_pGraph) { g_pGraph->Release();  g_pGraph = nullptr; }
        DestroyWindow(g_hwnd);
        CoUninitialize();
        return 1;
    }

    // 隐藏鼠标
    ShowCursor(FALSE);

    // 消息循环
    bool bRunning = true;
    MSG msg = { 0 };
    long evCode = 0;

    while (bRunning)
    {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                bRunning = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        ShowCursor(FALSE);

        if (g_pEvent && SUCCEEDED(g_pEvent->WaitForCompletion(100, &evCode)))
        {
            if (evCode == EC_COMPLETE || evCode == EC_USERABORT)
            {
                bRunning = false;
            }
        }
        else
        {
            if (!IsWindow(g_hwnd))
                bRunning = false;
        }
    }

    ShowCursor(TRUE);

    // 清理
    if (g_pControl) g_pControl->Stop();
    if (g_pVidWin)
    {
        g_pVidWin->put_Visible(OAFALSE);
        g_pVidWin->put_Owner(NULL);
        g_pVidWin->Release();
        g_pVidWin = nullptr;
    }
    if (g_pEvent) { g_pEvent->Release();  g_pEvent = nullptr; }
    if (g_pControl) { g_pControl->Release();g_pControl = nullptr; }
    if (g_pGraph) { g_pGraph->Release();  g_pGraph = nullptr; }
    DestroyWindow(g_hwnd);
    CoUninitialize();
    return 0;
}

// 主入口
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    bool playMode = false;
    bool uninstallMode = false;

    int nArgs;
    LPWSTR* szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    for (int i = 1; i < nArgs; ++i)
    {
        if (_wcsicmp(szArglist[i], L"/play") == 0)
            playMode = true;
        else if (_wcsicmp(szArglist[i], L"/uninstall") == 0)
            uninstallMode = true;
    }
    LocalFree(szArglist);

    if (uninstallMode)
    {
        if (!IsRunAsAdmin())
        {
            WCHAR exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            ShellExecuteW(nullptr, L"runas", exePath, L"/uninstall", nullptr, SW_SHOWNORMAL);
            return 0;
        }
        // 清理旧的、无效的开机计划任务（SYSTEM 在 Session 0 运行，登录前无法显示）
        DeleteStartupTask();
        // 移除视频同步器开机自启（并结束后台进程）
        RemoveUpdateAutostart();
        // 注销登录前视频 Credential Provider
        std::wstring dllPath = GetExeDirectory() + L"\\ValveLogonVideo.dll";
        if (GetFileAttributesW(dllPath.c_str()) != INVALID_FILE_ATTRIBUTES)
            UnregisterLogonProvider(dllPath);
        // 恢复 Windows 锁屏界面（安装时被关闭）
        RegDeleteKeyValueW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization", L"NoLockScreen");
        MessageBoxW(nullptr, L"登录前视频已取消。", L"Valve Boot", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (playMode)
    {
        std::wstring tempVideo = ExtractVideoResource();
        if (tempVideo.empty())
        {
            tempVideo = GetExeDirectory() + L"\\startup.wmv";
            if (GetFileAttributesW(tempVideo.c_str()) == INVALID_FILE_ATTRIBUTES)
                return 1;
        }

        int result = RunPlaybackMode(tempVideo);

        if (tempVideo.find(L"ValveBoot_startup.wmv") != std::wstring::npos)
            DeleteFileW(tempVideo.c_str());

        return result;
    }

    // 设置模式：一键注册（不预览动画），直接完成全部安装
    // 预览请用 /play 参数（Valve Boot.exe /play）
    if (!IsRunAsAdmin())
    {
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        ShellExecuteW(nullptr, L"runas", exePath, L"", nullptr, SW_SHOWNORMAL);
        return 0;
    }

    // 旧的计划任务方案在登录前无法显示（SYSTEM 跑在 Session 0），先清理
    DeleteStartupTask();

    // 注册登录前视频（Credential Provider）——由 LogonUI 在登录界面播放内嵌视频
    std::wstring dllPath = GetExeDirectory() + L"\\ValveLogonVideo.dll";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        MessageBoxW(nullptr, L"未找到 ValveLogonVideo.dll，无法安装登录前视频。", L"Valve Boot", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (RegisterLogonProvider(dllPath))
    {
        // 关闭 Windows 锁屏界面：开机时 LockScreen（壁纸+时钟）会在登录前闪现，
        // 凭据提供程序无法在它之前运行，只能用系统策略关闭。
        DWORD dwOne = 1;
        RegSetKeyValueW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization",
            L"NoLockScreen", REG_DWORD, &dwOne, sizeof(dwOne));

        // 创建视频文件夹（多视频轮播）与默认配置文件（可热修改，无需重编译）
        std::wstring exeDir = GetExeDirectory();
        CreateDirectoryW((exeDir + L"\\videos").c_str(), nullptr);
        std::wstring iniPath = exeDir + L"\\config.ini";
        if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            WritePrivateProfileStringW(L"Video", L"Mode", L"sequential", iniPath.c_str());
            WritePrivateProfileStringW(L"Video", L"VideoFolder", L"videos", iniPath.c_str());
            WritePrivateProfileStringW(L"Video", L"HoldMs", L"60000", iniPath.c_str());
        }
        // 补齐 [Update] 段默认值（GitHub 视频自动同步），已有配置不动
        {
            wchar_t tmp[32] = { 0 };
            GetPrivateProfileStringW(L"Update", L"Enabled", L"", tmp, 32, iniPath.c_str());
            if (tmp[0] == 0)
            {
                WritePrivateProfileStringW(L"Update", L"Enabled", L"1", iniPath.c_str());
                WritePrivateProfileStringW(L"Update", L"Repo", L"GarlicXP/Valve-Boot", iniPath.c_str());
                WritePrivateProfileStringW(L"Update", L"RepoFolder", L"video", iniPath.c_str());
                WritePrivateProfileStringW(L"Update", L"IntervalMin", L"30", iniPath.c_str());
            }
        }
        // 注册视频同步器开机自启（登录后后台检查 GitHub video 文件夹）
        AddUpdateAutostart(exeDir);

        MessageBoxW(nullptr,
            L"登录前视频已设置成功！\n"
            L"· 将视频文件放入 videos 文件夹（wmv/mp4/avi/mkv/mov/mpg，每次开机播放一个）\n"
            L"· 编辑 config.ini 可切换顺序/随机模式（默认顺序循环）\n"
            L"· 已关闭系统锁屏界面，避免开机闪现\n"
            L"· 已启用 GitHub 自动同步：每次登录后及每 30 分钟检查一次仓库 video 文件夹，\n"
            L"  新增/变化的视频会自动下载到本目录 videos（下一轮开机生效）",
            L"Valve Boot", MB_OK | MB_ICONINFORMATION);
    }
    else
        MessageBoxW(nullptr, L"登录前视频安装失败。", L"Valve Boot", MB_OK | MB_ICONERROR);
    return 0;
}