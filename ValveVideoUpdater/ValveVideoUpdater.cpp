// ValveVideoUpdater.cpp : 登录前视频自动同步器（后台运行，无窗口）。
//
// 作用：应用启用（安装）后，每次登录时启动一次，之后每 30 分钟检查一次
// GitHub 仓库指定文件夹（默认 video）中的视频文件；把本地 videos 文件夹里
// 缺失或内容不同的视频下载下来，达到"更新视频后无需重装、自动实时更新"。
//
// 配置：与登录前视频 DLL 共用同目录下的 config.ini 的 [Update] 段，
// 每次检查前都会重新读取，热修改立即生效（无需重启）：
//   [Update]
//   Enabled=1                 ; 1=启用同步 0=关闭
//   Repo=GarlicXP/Valve-Boot  ; GitHub 仓库（owner/repo）
//   RepoFolder=video          ; 仓库中存放视频的文件夹
//   IntervalMin=30            ; 检查间隔（分钟）
//
// 行为：
//   * 无参数：若开机自启项存在（已安装）则循环运行；否则只执行一次后退出
//     （手动双击 = 手动同步一次）。
//   * 参数 once：执行一次后退出（调试用）。
//   * 参数 -sha <文件>：计算文件的 Git blob SHA1 并写入日志后退出（自检用）。
//   * 只下载、更新，从不删除本地 videos 里的文件（保护用户的本地视频）。
//   * 运行记录写入同目录 sync.log。
//
// 差异判断：GitHub Contents API 返回每个文件的 sha（Git blob SHA1）与大小；
// 本地文件大小不同直接判为需要更新，大小相同再计算本地 blob SHA1 比较，
// 避免下载未变化的视频。

#include <windows.h>
#include <wininet.h>
#include <bcrypt.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shlwapi.lib")

// ---------------------------------------------------------------------------
// 基础工具
// ---------------------------------------------------------------------------

static std::wstring GetExeDir()
{
    WCHAR buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) return L"";
    std::wstring s(buf, n);
    size_t p = s.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L"";
    return s.substr(0, p);
}

// 追加一行到 sync.log（UTF-8 带 BOM），日志超过 256KB 时重置
static void Log(const wchar_t* fmt, ...)
{
    std::wstring dir = GetExeDir();
    std::wstring path = dir + L"\\sync.log";

    // 控制日志大小
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
        fad.nFileSizeLow > 256 * 1024)
        DeleteFileW(path.c_str());

    wchar_t line[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(line, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[64];
    swprintf_s(ts, L"[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring text = std::wstring(ts) + line + L"\r\n";
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return;
    std::string bytes(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &bytes[0], len, nullptr, nullptr);

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    if (GetFileSize(h, nullptr) == 0)
    {
        // 写入 BOM，方便记事本识别 UTF-8
        const BYTE bom[] = { 0xEF, 0xBB, 0xBF };
        DWORD w = 0;
        WriteFile(h, bom, 3, &w, nullptr);
    }
    DWORD written = 0;
    WriteFile(h, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    CloseHandle(h);
}

// 手写 INI 读取（兼容 UTF-8 BOM / ANSI / 任意换行；与 DLL 端同一套逻辑）
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

    if (read >= 3 && (BYTE)data[0] == 0xEF && (BYTE)data[1] == 0xBB && (BYTE)data[2] == 0xBF)
    {
        data.erase(0, 3);
        read -= 3;
    }

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
        if (line[first] == L';') continue;
        if (line[first] == L'[')
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
        while (!k.empty() && (k.front() == L' ' || k.front() == L'\t')) k.erase(k.begin());
        if (k != key) continue;

        std::wstring v = line.substr(eq + 1);
        size_t vs = v.find_first_not_of(L" \t");
        if (vs != std::wstring::npos) v = v.substr(vs);
        size_t ve = v.find_last_not_of(L" \t\r");
        if (ve != std::wstring::npos) v = v.substr(0, ve + 1);
        else v.clear();
        result = v;
        break;
    }
    return result;
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

struct UpdateConfig
{
    bool enabled = true;
    std::wstring repo = L"GarlicXP/Valve-Boot";
    std::wstring repoFolder = L"video";
    long intervalMin = 30;
};

static bool ValidRepo(const std::wstring& s)
{
    // 仅允许 owner/repo，且只含安全字符，防止拼接出异常 URL
    size_t slash = s.find(L'/');
    if (slash == std::wstring::npos || s.find(L'/', slash + 1) != std::wstring::npos) return false;
    for (wchar_t c : s)
    {
        if (isalnum((unsigned char)c) || c == L'-' || c == L'_' || c == L'.' || c == L'/') continue;
        return false;
    }
    return true;
}

static bool ValidFolder(const std::wstring& s)
{
    if (s.empty() || s.find(L"..") != std::wstring::npos || s.find(L'/') != std::wstring::npos) return false;
    for (wchar_t c : s)
    {
        if (isalnum((unsigned char)c) || c == L'-' || c == L'_' || c == L'.') continue;
        return false;
    }
    return true;
}

static UpdateConfig LoadConfig(const std::wstring& exeDir)
{
    UpdateConfig cfg;
    std::wstring ini = exeDir + L"\\config.ini";

    std::wstring en = IniGetString(ini, L"Update", L"Enabled", L"1");
    cfg.enabled = (en != L"0" && _wcsicmp(en.c_str(), L"false") != 0);

    std::wstring repo = IniGetString(ini, L"Update", L"Repo", cfg.repo);
    if (ValidRepo(repo)) cfg.repo = repo;

    std::wstring folder = IniGetString(ini, L"Update", L"RepoFolder", cfg.repoFolder);
    if (ValidFolder(folder)) cfg.repoFolder = folder;

    std::wstring ms = IniGetString(ini, L"Update", L"IntervalMin", L"");
    if (!ms.empty())
    {
        long v = wcstol(ms.c_str(), nullptr, 10);
        if (v >= 1 && v <= 1440) cfg.intervalMin = v;
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// 网络（WinINet）
// ---------------------------------------------------------------------------

// GET 文本（用于 GitHub Contents API），返回 HTTP 状态码与响应体
static bool HttpGetText(const std::wstring& url, std::string& outBody, DWORD& outStatus)
{
    outBody.clear();
    outStatus = 0;
    HINTERNET hNet = InternetOpenW(L"ValveBoot-Updater/1.0", INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr, nullptr, 0);
    if (!hNet) return false;
    HINTERNET hUrl = InternetOpenUrlW(hNet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
    if (!hUrl)
    {
        InternetCloseHandle(hNet);
        return false;
    }
    DWORD status = 0, cb = sizeof(status);
    if (!HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &cb, nullptr))
        status = 0;
    outStatus = status;
    char buf[8192];
    DWORD read = 0;
    while (InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0)
    {
        outBody.append(buf, read);
        read = 0;
    }
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hNet);
    return true;
}

// 下载文件到指定路径；成功返回 true（已写入完整文件）
static bool HttpDownload(const std::wstring& url, const std::wstring& path)
{
    HINTERNET hNet = InternetOpenW(L"ValveBoot-Updater/1.0", INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr, nullptr, 0);
    if (!hNet) return false;
    HINTERNET hUrl = InternetOpenUrlW(hNet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
    if (!hUrl)
    {
        InternetCloseHandle(hNet);
        return false;
    }
    DWORD status = 0, cb = sizeof(status);
    if (HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &cb, nullptr) &&
        status != 200)
    {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hNet);
        return false;
    }

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hNet);
        return false;
    }
    char buf[65536];
    DWORD read = 0;
    bool ok = true;
    while (InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0)
    {
        DWORD written = 0;
        if (!WriteFile(hFile, buf, read, &written, nullptr) || written != read)
        {
            ok = false;
            break;
        }
        read = 0;
    }
    CloseHandle(hFile);
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hNet);
    return ok;
}

// ---------------------------------------------------------------------------
// Git blob SHA1（与 GitHub 的 sha 字段一致，用于判断文件是否变化）
// ---------------------------------------------------------------------------

static std::string GitBlobSha(const std::wstring& path, unsigned long long fileSize)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return "";

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0)
    {
        if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(hFile);
        return "";
    }

    // Git blob 头："blob <size>\0"。注意不能用 + "\0" 追加（字面量会被转成
    // 空 std::string），必须 push_back('\0') 才能保证 NUL 字节进入哈希。
    std::string header = "blob " + std::to_string(fileSize);
    header.push_back('\0');
    BCryptHashData(hHash, (PUCHAR)header.data(), (ULONG)header.size(), 0);

    char buf[65536];
    DWORD read = 0;
    while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read > 0)
    {
        BCryptHashData(hHash, (PUCHAR)buf, read, 0);
        read = 0;
    }
    CloseHandle(hFile);

    BYTE digest[20];
    BCryptFinishHash(hHash, digest, 20, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (int i = 0; i < 20; ++i)
    {
        out += hex[digest[i] >> 4];
        out += hex[digest[i] & 0xF];
    }
    return out;
}

// ---------------------------------------------------------------------------
// GitHub Contents API 响应解析（轻量 JSON 字段提取，不引入第三方库）
// ---------------------------------------------------------------------------

struct RemoteEntry
{
    std::string name;
    std::string sha;
    unsigned long long size = 0;
};

static size_t FindObjectEnd(const std::string& s, size_t open)
{
    int depth = 0;
    bool inStr = false;
    for (size_t i = open; i < s.size(); ++i)
    {
        char c = s[i];
        if (inStr)
        {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inStr = false;
        }
        else
        {
            if (c == '"') inStr = true;
            else if (c == '{') ++depth;
            else if (c == '}') { --depth; if (depth == 0) return i; }
        }
    }
    return std::string::npos;
}

static bool JsonStr(const std::string& obj, const char* key, std::string& out)
{
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    size_t e = obj.find('"', p);
    if (e == std::string::npos) return false;
    out = obj.substr(p, e - p);
    return true;
}

static bool JsonNum(const std::string& obj, const char* key, unsigned long long& out)
{
    std::string pat = std::string("\"") + key + "\":";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    size_t e = p;
    while (e < obj.size() && isdigit((unsigned char)obj[e])) ++e;
    if (e == p) return false;
    out = strtoull(obj.substr(p, e - p).c_str(), nullptr, 10);
    return true;
}

static void ParseContents(const std::string& body, std::vector<RemoteEntry>& out)
{
    size_t p = 0;
    while ((p = body.find('{', p)) != std::string::npos)
    {
        size_t end = FindObjectEnd(body, p);
        if (end == std::string::npos) break;
        std::string obj = body.substr(p, end - p + 1);
        std::string type, name, sha;
        unsigned long long size = 0;
        if (JsonStr(obj, "type", type) && type == "file" &&
            JsonStr(obj, "name", name) && JsonStr(obj, "sha", sha) &&
            JsonNum(obj, "size", size))
        {
            RemoteEntry e;
            e.name = name;
            e.sha = sha;
            e.size = size;
            out.push_back(e);
        }
        p = end + 1;
    }
}

// ---------------------------------------------------------------------------
// 同步逻辑
// ---------------------------------------------------------------------------

static bool IsVideoExt(const std::string& name)
{
    static const char* kExts[] = { ".wmv", ".mp4", ".avi", ".mkv", ".mov", ".mpg", ".mpeg" };
    for (const char* ext : kExts)
    {
        size_t el = strlen(ext);
        if (name.size() >= el)
        {
            std::string tail = name.substr(name.size() - el);
            for (auto& c : tail) c = (char)tolower((unsigned char)c);
            if (tail == ext) return true;
        }
    }
    return false;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}

// 过滤 Windows 文件名非法字符
static std::wstring SanitizeName(const std::wstring& name)
{
    std::wstring out = name;
    for (auto& c : out)
    {
        if (c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' ||
            c == L'\\' || c == L'|' || c == L'?' || c == L'*')
            c = L'_';
    }
    return out;
}

static std::string UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// 执行一次同步；返回：0=正常完成，-1=网络/服务异常（稍后重试）
static int SyncOnce(const std::wstring& exeDir, const UpdateConfig& cfg)
{
    std::wstring videosDir = exeDir + L"\\videos";
    CreateDirectoryW(videosDir.c_str(), nullptr);

    std::wstring apiUrl = L"https://api.github.com/repos/" + cfg.repo +
        L"/contents/" + cfg.repoFolder;
    std::string body;
    DWORD status = 0;
    if (!HttpGetText(apiUrl, body, status))
    {
        Log(L"[同步] 网络请求失败（可能未联网或代理异常），稍后自动重试");
        return -1;
    }
    if (status == 403)
    {
        Log(L"[同步] GitHub API 限流(403)，稍后自动重试");
        return -1;
    }
    if (status == 404)
    {
        Log(L"[同步] 仓库或文件夹不存在(404)：%s/%s", cfg.repo.c_str(), cfg.repoFolder.c_str());
        return 0;
    }
    if (status != 200)
    {
        Log(L"[同步] GitHub API 返回意外状态码 %lu", status);
        return -1;
    }

    std::vector<RemoteEntry> entries;
    ParseContents(body, entries);
    int count = 0;
    for (const auto& e : entries)
        if (IsVideoExt(e.name)) ++count;
    if (count == 0)
    {
        Log(L"[同步] 仓库 %s 的 %s 文件夹里没有视频文件", cfg.repo.c_str(), cfg.repoFolder.c_str());
        return 0;
    }

    int updated = 0, skipped = 0, failed = 0;
    for (const auto& e : entries)
    {
        if (!IsVideoExt(e.name)) continue;
        std::wstring safe = SanitizeName(Utf8ToWide(e.name));
        if (safe.empty()) continue;
        std::wstring local = videosDir + L"\\" + safe;

        // 判断是否需要下载：大小不同 → 需要；大小相同 → 比较 blob SHA1
        bool need = true;
        HANDLE hLocal = CreateFileW(local.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hLocal != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER sz;
            GetFileSizeEx(hLocal, &sz);
            CloseHandle(hLocal);
            if ((unsigned long long)sz.QuadPart == e.size)
            {
                std::string sha = GitBlobSha(local, e.size);
                if (!sha.empty() && sha == e.sha) need = false;
            }
        }
        if (!need)
        {
            ++skipped;
            continue;
        }

        // 下载到临时文件（隐藏 .part，播放器不会选中），成功后原子替换
        std::wstring tmp = videosDir + L"\\." + safe + L".part";
        DeleteFileW(tmp.c_str());
        std::string nameUtf8 = e.name;
        std::wstring dlUrl = L"https://raw.githubusercontent.com/" + cfg.repo +
            L"/HEAD/" + cfg.repoFolder + L"/" + Utf8ToWide(UrlEncode(nameUtf8));
        if (!HttpDownload(dlUrl, tmp))
        {
            DeleteFileW(tmp.c_str());
            Log(L"[同步] 下载失败：%s", safe.c_str());
            ++failed;
            continue;
        }

        // 校验下载大小与 GitHub 记录一致
        HANDLE hTmp = CreateFileW(tmp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        unsigned long long dlSize = 0;
        if (hTmp != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(hTmp, &sz) && sz.QuadPart >= 0) dlSize = (unsigned long long)sz.QuadPart;
            CloseHandle(hTmp);
        }
        if (dlSize != e.size)
        {
            DeleteFileW(tmp.c_str());
            Log(L"[同步] 下载不完整（%llu/%llu 字节），已丢弃：%s", dlSize, e.size, safe.c_str());
            ++failed;
            continue;
        }

        if (!MoveFileExW(tmp.c_str(), local.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(tmp.c_str());
            Log(L"[同步] 写入本地失败：%s", safe.c_str());
            ++failed;
            continue;
        }
        ++updated;
        Log(L"[同步] 已更新视频：%s（%llu 字节）", safe.c_str(), e.size);
    }

    Log(L"[同步] 完成：新增/更新 %d 个，已最新 %d 个，失败 %d 个", updated, skipped, failed);
    return 0;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

static bool RunKeyExists()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, cb = 0;
    LONG r = RegQueryValueExW(hKey, L"ValveVideoUpdater", nullptr, &type, nullptr, &cb);
    RegCloseKey(hKey);
    return r == ERROR_SUCCESS;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    std::wstring exeDir = GetExeDir();

    bool once = false;
    std::wstring shaFile;
    int nArgs = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (argv)
    {
        for (int i = 1; i < nArgs; ++i)
        {
            if (_wcsicmp(argv[i], L"once") == 0) once = true;
            else if ((_wcsicmp(argv[i], L"-sha") == 0 || _wcsicmp(argv[i], L"/sha") == 0) && i + 1 < nArgs)
            {
                shaFile = argv[i + 1];
                ++i;
            }
        }
        LocalFree(argv);
    }

    // 自检模式：计算指定文件的 Git blob SHA1
    if (!shaFile.empty())
    {
        HANDLE hF = CreateFileW(shaFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hF == INVALID_HANDLE_VALUE)
        {
            Log(L"[自检] 无法打开文件：%s", shaFile.c_str());
            return 1;
        }
        LARGE_INTEGER sz;
        GetFileSizeEx(hF, &sz);
        CloseHandle(hF);
        std::string sha = GitBlobSha(shaFile, (unsigned long long)sz.QuadPart);
        Log(L"[自检] SHA1(%s) = %s", shaFile.c_str(), sha.empty() ? L"<失败>" : Utf8ToWide(sha).c_str());
        return 0;
    }

    bool installed = RunKeyExists();
    Log(L"[启动] ValveVideoUpdater v1.0，目录：%s，自启=%s，once=%s",
        exeDir.c_str(), installed ? L"是" : L"否", once ? L"是" : L"否");

    for (;;)
    {
        UpdateConfig cfg = LoadConfig(exeDir);
        if (!ValidRepo(cfg.repo) || !ValidFolder(cfg.repoFolder))
        {
            Log(L"[同步] 配置无效（Repo/RepoFolder），请检查 config.ini 的 [Update] 段");
        }
        else if (!cfg.enabled)
        {
            Log(L"[同步] 已禁用（[Update] Enabled=0），如需启用请修改 config.ini");
        }
        else
        {
            Log(L"[同步] 开始检查 %s/%s ...", cfg.repo.c_str(), cfg.repoFolder.c_str());
            SyncOnce(exeDir, cfg);
        }

        if (once) break;

        // 自启项已被删除（例如用户卸载或手动移除）→ 结束运行
        if (!RunKeyExists())
        {
            Log(L"[退出] 开机自启项已移除，停止后台运行");
            break;
        }

        DWORD interval = (DWORD)((cfg.intervalMin > 0 ? cfg.intervalMin : 30) * 60000ULL);
        Sleep(interval);
    }
    return 0;
}
