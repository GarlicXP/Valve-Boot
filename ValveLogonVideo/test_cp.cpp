// test_cp.cpp : Credential Provider 逻辑验证测试壳
// 用法：
//   test_cp.exe            - 仅验证 COM 逻辑（不启动视频）
//   test_cp.exe play       - 模拟 CPUS_LOGON，启动全屏视频（当前桌面可见，用于视觉验证）
#include <windows.h>
#include <credentialprovider.h>
#include <stdio.h>

// 与 ValveLogonVideo.cpp 中一致
static const CLSID CLSID_ValveLogonVideo =
{ 0x98deb984, 0x1c7e, 0x4531, { 0xb5, 0xd2, 0x9d, 0x32, 0x96, 0xb4, 0x16, 0xf9 } };

typedef HRESULT(STDAPICALLTYPE* DllGetClassObjectFn)(REFCLSID, REFIID, void**);

static void PrintHr(const char* what, HRESULT hr)
{
    printf("%-28s -> 0x%08lX %s\n", what, (unsigned long)hr,
        SUCCEEDED(hr) ? "(OK)" : "(FAILED)");
}

int wmain(int argc, wchar_t* argv[])
{
    bool bPlay = (argc > 1 && wcscmp(argv[1], L"play") == 0);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PrintHr("CoInitializeEx", hr);

    HMODULE hDll = LoadLibraryW(L"ValveLogonVideo.dll");
    if (!hDll) { printf("LoadLibrary(ValveLogonVideo.dll) FAILED (%lu)\n", GetLastError()); return 1; }
    printf("LoadLibrary OK\n");

    DllGetClassObjectFn pfn = (DllGetClassObjectFn)GetProcAddress(hDll, "DllGetClassObject");
    if (!pfn) { printf("GetProcAddress(DllGetClassObject) FAILED\n"); return 1; }

    IClassFactory* pFactory = nullptr;
    hr = pfn(CLSID_ValveLogonVideo, IID_IClassFactory, (void**)&pFactory);
    PrintHr("DllGetClassObject(IClassFactory)", hr);
    if (FAILED(hr)) return 1;

    ICredentialProvider* pCP = nullptr;
    hr = pFactory->CreateInstance(nullptr, IID_ICredentialProvider, (void**)&pCP);
    PrintHr("CreateInstance(ICredentialProvider)", hr);
    if (FAILED(hr)) { pFactory->Release(); return 1; }

    DWORD t0 = GetTickCount();
    hr = pCP->SetUsageScenario(bPlay ? CPUS_LOGON : CPUS_UNLOCK_WORKSTATION, 0);
    DWORD dt = GetTickCount() - t0;
    PrintHr("SetUsageScenario", hr);
    printf("  SetUsageScenario 耗时: %lu 毫秒%s\n", dt,
        bPlay ? "（应约为视频时长 12800ms，说明已阻塞到视频播完）" : "（解锁场景不阻塞，应很小）");

    DWORD dwCount = 999, dwDefault = 999, dwFields = 999;
    BOOL bAuto = TRUE;
    hr = pCP->GetCredentialCount(&dwCount, &dwDefault, &bAuto);
    PrintHr("GetCredentialCount", hr);
    printf("  credentialCount=%lu default=%lu autoLogon=%s\n",
        dwCount, dwDefault, bAuto ? "TRUE" : "FALSE");

    hr = pCP->GetFieldDescriptorCount(&dwFields);
    PrintHr("GetFieldDescriptorCount", hr);
    printf("  fieldCount=%lu\n", dwFields);

    if (bPlay)
    {
        // 视频播放线程已异步启动；等待视频结束（视频约 13 秒）
        printf(">> 视频播放线程已启动（模拟登录场景），等待 17 秒让视频播完...\n");
        Sleep(17000);
    }

    pCP->Release();
    pFactory->Release();
    FreeLibrary(hDll);
    CoUninitialize();
    printf("测试完成。\n");
    return 0;
}
