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
    bool autoUpdate = true;   // [Update] AutoUpdate=1：启用软件自更新（GitHub Releases）
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

    std::wstring au = IniGetString(ini, L"Update", L"AutoUpdate", L"1");
    cfg.autoUpdate = (au != L"0" && _wcsicmp(au.c_str(), L"false") != 0);

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

// 单通道单次下载；成功返回 true（已写入完整文件）。
static bool HttpDownloadOnce(const std::wstring& url, const std::wstring& path)
{
    HINTERNET hNet = InternetOpenW(L"ValveBoot-Updater/1.0", INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr, nullptr, 0);
    if (!hNet)
    {
        Log(L"[下载] %s 网络初始化失败", path.c_str());
        return false;
    }
    HINTERNET hUrl = InternetOpenUrlW(hNet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
    if (!hUrl)
    {
        DWORD err = GetLastError();
        Log(L"[下载] %s 打开连接失败（错误 %lu）", path.c_str(), err);
        InternetCloseHandle(hNet);
        return false;
    }
    DWORD status = 0, cb = sizeof(status);
    if (HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &cb, nullptr) &&
        status != 200)
    {
        // 404=文件已不在仓库；403=限流/无权限；5xx=GitHub 临时故障
        Log(L"[下载] %s HTTP %lu", path.c_str(), status);
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hNet);
        return false;
    }

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        DWORD err = GetLastError();
        Log(L"[下载] %s 无法创建本地文件（错误 %lu）", path.c_str(), err);
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
    if (!ok)
        Log(L"[下载] %s 传输中断", path.c_str());
    return ok;
}

// 多通道 + 多轮重试下载：raw.githubusercontent 在国内常不稳定，
// 自动依次尝试 raw → jsDelivr CDN → github.com/raw，最多 3 轮。
static bool HttpDownload(const std::vector<std::wstring>& urls, const std::wstring& path)
{
    for (int round = 1; round <= 3; ++round)
    {
        for (size_t i = 0; i < urls.size(); ++i)
        {
            if (round > 1 || i > 0)
            {
                Log(L"[下载] %s 切换通道/重试（通道 %d，第 %d 轮，等待 3 秒）...",
                    path.c_str(), (int)(i + 1), round);
                Sleep(3000);
            }
            if (HttpDownloadOnce(urls[i], path))
                return true;
        }
    }
    return false;
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
        std::wstring nameEnc = Utf8ToWide(UrlEncode(nameUtf8));
        std::wstring base = cfg.repo + L"/HEAD/" + cfg.repoFolder + L"/" + nameEnc;
        std::vector<std::wstring> urls;
        urls.push_back(L"https://raw.githubusercontent.com/" + base);
        urls.push_back(L"https://cdn.jsdelivr.net/gh/" + cfg.repo + L"@HEAD/" + cfg.repoFolder + L"/" + nameEnc);
        urls.push_back(L"https://github.com/" + base);
        if (!HttpDownload(urls, tmp))
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
        // .part 临时文件带隐藏属性，改名后会被继承，导致用户看不到
        // 已下载的视频；这里显式清除，恢复正常可见。
        SetFileAttributesW(local.c_str(), FILE_ATTRIBUTE_NORMAL);
        ++updated;
        Log(L"[同步] 已更新视频：%s（%llu 字节）", safe.c_str(), e.size);
    }

    Log(L"[同步] 完成：新增/更新 %d 个，已最新 %d 个，失败 %d 个", updated, skipped, failed);
    return 0;
}

// 是否以管理员（提权）令牌运行
static bool IsElevated()
{
    BOOL elevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        TOKEN_ELEVATION te = { 0 };
        DWORD cb = 0;
        if (GetTokenInformation(hToken, TokenElevation, &te, sizeof(te), &cb))
            elevated = te.TokenIsElevated;
        CloseHandle(hToken);
    }
    return elevated != FALSE;
}

// 静默执行 schtasks（CREATE_NO_WINDOW），返回是否成功（退出码 0）
static bool SchtasksCmd(const std::wstring& args)
{
    std::wstring cmd = L"schtasks " + args;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

static const wchar_t* kTaskName = L"ValveBootVideoUpdater";

static bool TaskExists()
{
    return SchtasksCmd(std::wstring(L"/Query /TN \"") + kTaskName + L"\"");
}

// 创建计划任务：登录时以最高权限（管理员）运行更新器，全程无 UAC 弹窗
static bool CreateTask(const std::wstring& exeDir)
{
    std::wstring exe = exeDir + L"\\ValveVideoUpdater.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring args = L"/Create /TN \"" + std::wstring(kTaskName) +
        L"\" /TR \"\\\"" + exe + L"\\\"\" /SC ONLOGON /RL HIGHEST /F";
    return SchtasksCmd(args);
}

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

static bool RemoveRunKey()
{
    return RegDeleteKeyValueW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"ValveVideoUpdater") == ERROR_SUCCESS;
}

// 自启是否已配置（计划任务 或 旧版 Run 键）
static bool IsAutostartInstalled()
{
    return TaskExists() || RunKeyExists();
}

// 升级到计划任务模式：创建任务并移除旧 Run 键（防双启动），需要管理员
static bool MigrateToTask(const std::wstring& exeDir)
{
    bool ok = false;
    if (!TaskExists())
        ok = CreateTask(exeDir);
    else
        ok = true;
    if (ok && RunKeyExists())
        RemoveRunKey();
    return ok;
}

// 静默注册登录前视频 DLL（regsvr32 /s，无窗口）；返回是否成功
static bool RegisterProvider(const std::wstring& dllPath)
{
    std::wstring cmd = L"regsvr32 /s \"" + dllPath + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

// 检查注册表：登录前视频 DLL 是否已注册
static bool IsProviderRegistered()
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Classes\\CLSID\\{98DEB984-1C7E-4531-B5D2-9D3296B416F9}\\InprocServer32",
        0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    RegCloseKey(hKey);
    return true;
}

// 自愈：DLL 存在但注册缺失时静默补注册（需管理员）
static void EnsureRegistered(const std::wstring& exeDir)
{
    std::wstring dll = exeDir + L"\\ValveLogonVideo.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (IsProviderRegistered()) return;
    if (RegisterProvider(dll))
        Log(L"[自愈] 登录前视频 DLL 注册缺失，已静默补注册");
    else
        Log(L"[自愈] 补注册失败（可能权限不足）");
}
// ---------------------------------------------------------------------------
// 软件自更新（GitHub Releases）
// ---------------------------------------------------------------------------

static std::wstring GetLocalVersion(const std::wstring& exeDir)
{
    std::wstring path = exeDir + L"\\version.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    char buf[64] = { 0 };
    DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    std::string t(buf, rd);
    while (!t.empty() && (t.back() == '\r' || t.back() == '\n' || t.back() == ' ')) t.pop_back();
    return Utf8ToWide(t);
}

static void SetLocalVersion(const std::wstring& exeDir, const std::wstring& tag)
{
    std::wstring path = exeDir + L"\\version.txt";
    int len = WideCharToMultiByte(CP_UTF8, 0, tag.c_str(), (int)tag.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return;
    std::string bytes(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, tag.c_str(), (int)tag.size(), &bytes[0], len, nullptr, nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wr, nullptr);
    CloseHandle(h);
}

// 解析 releases/latest 响应：tag_name 与 ValveBoot-Update.zip 的下载地址
static bool ParseLatestRelease(const std::string& body, std::string& tag, std::string& zipUrl)
{
    tag.clear();
    zipUrl.clear();
    JsonStr(body, "tag_name", tag);
    if (tag.empty()) return false;
    size_t p = 0;
    while ((p = body.find('{', p)) != std::string::npos)
    {
        size_t end = FindObjectEnd(body, p);
        if (end == std::string::npos) break;
        std::string obj = body.substr(p, end - p + 1);
        std::string name, url;
        if (JsonStr(obj, "name", name) && name == "ValveBoot-Update.zip" &&
            JsonStr(obj, "browser_download_url", url) && !url.empty())
        {
            zipUrl = url;
            return true;
        }
        p = end + 1;
    }
    return false;
}

// 用系统自带 tar.exe 解压 zip（Windows 10 1803+ 内置）
static bool UnzipWithTar(const std::wstring& zip, const std::wstring& outDir)
{
    std::wstring cmd = L"\"C:\\Windows\\System32\\tar.exe\" -xf \"" + zip +
        L"\" -C \"" + outDir + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

// 执行一次软件自更新；返回 true 表示已应用更新（调用方应退出，等待自身替换重启）
static bool SelfUpdateOnce(const std::wstring& exeDir, const UpdateConfig& cfg)
{
    if (!cfg.autoUpdate)
    {
        Log(L"[更新] 软件自更新已禁用（[Update] AutoUpdate=0）");
        return false;
    }

    std::wstring apiUrl = L"https://api.github.com/repos/" + cfg.repo + L"/releases/latest";
    std::string body;
    DWORD status = 0;
    if (!HttpGetText(apiUrl, body, status))
    {
        Log(L"[更新] Releases 检查网络失败，稍后自动重试");
        return false;
    }
    if (status == 404)
    {
        Log(L"[更新] 仓库暂无 Release（首次发布后自动生效）");
        return false;
    }
    if (status != 200)
    {
        Log(L"[更新] Releases API 返回状态码 %lu", status);
        return false;
    }

    std::string tag, zipUrl;
    if (!ParseLatestRelease(body, tag, zipUrl))
    {
        if (!tag.empty())
            Log(L"[更新] 最新 Release %s 未附带 ValveBoot-Update.zip 资产，忽略（发布自更新包时请将资产命名为 ValveBoot-Update.zip）",
                Utf8ToWide(tag).c_str());
        else
            Log(L"[更新] 解析 Release 失败");
        return false;
    }

    std::wstring wTag = Utf8ToWide(tag);
    std::wstring localVer = GetLocalVersion(exeDir);
    if (!localVer.empty() && localVer == wTag)
        return false;   // 已是最新
    Log(L"[更新] 发现新版本 %s（本地 %s），开始下载", wTag.c_str(),
        localVer.empty() ? L"<首次>" : localVer.c_str());

    std::wstring zipPath = exeDir + L"\\.valveboot_update.zip";
    std::wstring tmpDir = exeDir + L"\\.update_tmp";
    DeleteFileW(zipPath.c_str());
    RemoveDirectoryW(tmpDir.c_str());
    CreateDirectoryW(tmpDir.c_str(), nullptr);

    std::vector<std::wstring> urls;
    urls.push_back(Utf8ToWide(zipUrl));
    if (!HttpDownload(urls, zipPath))
    {
        DeleteFileW(zipPath.c_str());
        Log(L"[更新] 更新包下载失败，保留当前版本，下轮自动重试");
        return false;
    }
    if (!UnzipWithTar(zipPath, tmpDir))
    {
        DeleteFileW(zipPath.c_str());
        Log(L"[更新] 解压失败（系统缺少 tar 或包损坏），跳过本轮");
        return false;
    }

    // 校验更新包包含播放 DLL，避免替换出空目录
    if (GetFileAttributesW((tmpDir + L"\\ValveLogonVideo.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        DeleteFileW(zipPath.c_str());
        Log(L"[更新] 更新包缺少 ValveLogonVideo.dll，已丢弃");
        return false;
    }

    // 替换文件（config.ini 与 videos\ 一律不动，保留用户配置与本地视频）
    bool needRestart = false;
    bool replacedDll = false;
    {
        std::wstring dst = exeDir + L"\\ValveLogonVideo.dll";
        if (CopyFileW((tmpDir + L"\\ValveLogonVideo.dll").c_str(), dst.c_str(), FALSE))
        {
            replacedDll = true;
            Log(L"[更新] 已替换 ValveLogonVideo.dll");
        }
        else
            Log(L"[更新] ValveLogonVideo.dll 替换失败（可能被占用），下轮重试");
    }
    if (GetFileAttributesW((tmpDir + L"\\Valve Boot.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if (CopyFileW((tmpDir + L"\\Valve Boot.exe").c_str(),
            (exeDir + L"\\Valve Boot.exe").c_str(), FALSE))
            Log(L"[更新] 已替换 Valve Boot.exe");
    }
    if (GetFileAttributesW((tmpDir + L"\\使用说明.txt").c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if (CopyFileW((tmpDir + L"\\使用说明.txt").c_str(),
            (exeDir + L"\\使用说明.txt").c_str(), FALSE))
            Log(L"[更新] 已替换 使用说明.txt");
    }
    // 自身（正在运行的 exe）：先复制为新文件，退出后延迟替换重启
    if (GetFileAttributesW((tmpDir + L"\\ValveVideoUpdater.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        std::wstring newExe = exeDir + L"\\ValveVideoUpdater.exe.new";
        DeleteFileW(newExe.c_str());
        if (CopyFileW((tmpDir + L"\\ValveVideoUpdater.exe").c_str(), newExe.c_str(), FALSE))
        {
            Log(L"[更新] 更新器自身将延迟替换并重启");
            needRestart = true;
        }
    }

    if (replacedDll)
    {
        // 新 DLL 已就位：静默重新注册（写 HKLM 需要管理员权限）
        if (IsElevated())
        {
            if (RegisterProvider(exeDir + L"\\ValveLogonVideo.dll"))
                Log(L"[更新] 新 DLL 已静默注册");
            else
                Log(L"[更新] 新 DLL 注册失败，将由下次开机自愈重试");
        }
        else
            Log(L"[更新] 非管理员，跳过注册（计划任务模式将自动以管理员重试）");
    }

    SetLocalVersion(exeDir, wTag);
    DeleteFileW(zipPath.c_str());
    // 清理解压目录（延迟到自身重启后，避免占用）；直接尝试删除即可
    {
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW((tmpDir + L"\\*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0)
                {
                    std::wstring fp = tmpDir + L"\\" + fd.cFileName;
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                        DeleteFileW(fp.c_str());
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        RemoveDirectoryW(tmpDir.c_str());
    }

    if (needRestart)
    {
        // 延迟 3 秒：等本进程退出后替换自身并重启
        std::wstring cmd = L"cmd /c timeout /t 3 /nobreak >nul & move /y \"" +
            (exeDir + L"\\ValveVideoUpdater.exe.new") + L"\" \"" +
            (exeDir + L"\\ValveVideoUpdater.exe") + L"\" & start \"\" \"" +
            (exeDir + L"\\ValveVideoUpdater.exe") + L"\"";
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    return needRestart;
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------


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

    // 单实例保护：多开会并发写同一个临时文件导致下载失败，
    // 后启动的实例直接退出（once 手动模式同样适用）。
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\ValveVideoUpdater_SingleInstance");
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    bool installed = IsAutostartInstalled();
    Log(L"[启动] ValveVideoUpdater v2.0（自更新版），目录：%s，自启=%s，管理员=%s，once=%s",
        exeDir.c_str(), installed ? L"是" : L"否",
        IsElevated() ? L"是" : L"否", once ? L"是" : L"否");

    // 非管理员且存在计划任务：通过计划任务以最高权限重启自身（无 UAC），随后退出
    // 让提升实例接管；计划任务不存在则继续以当前权限运行（视频同步不受影响）。
    if (!once && !IsElevated() && TaskExists())
    {
        if (SchtasksCmd(std::wstring(L"/Run /TN \"") + kTaskName + L"\""))
        {
            Log(L"[启动] 以普通权限启动，已请求计划任务以管理员身份重启");
            return 0;
        }
        Log(L"[启动] 计划任务重启失败，以当前权限继续（注册/自愈受限）");
    }

    // 管理员环境：补齐自启（升级到计划任务模式）并自愈注册
    if (IsElevated())
    {
        if (!TaskExists())
        {
            if (CreateTask(exeDir))
            {
                Log(L"[启动] 已创建计划任务自启（登录时最高权限运行）");
                RemoveRunKey();
            }
            else
                Log(L"[启动] 创建计划任务失败");
        }
        else if (RunKeyExists())
        {
            RemoveRunKey();   // 旧 Run 键与计划任务并存会导致双启动，清理
            Log(L"[启动] 已移除旧版 Run 自启（计划任务模式）");
        }
        EnsureRegistered(exeDir);
    }

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

        // 软件自更新（GitHub Releases 最新版，下载替换 + 静默注册）
        if (SelfUpdateOnce(exeDir, cfg))
        {
            Log(L"[更新] 更新已应用，等待延迟替换自身后重启");
            break;
        }

        if (once) break;

        // 自启项已被删除（例如用户卸载或手动移除）→ 结束运行
        if (!IsAutostartInstalled())
        {
            Log(L"[退出] 开机自启项已移除，停止后台运行");
            break;
        }

        DWORD interval = (DWORD)((cfg.intervalMin > 0 ? cfg.intervalMin : 30) * 60000ULL);
        Sleep(interval);
    }
    CloseHandle(hMutex);
    return 0;
}
