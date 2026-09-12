#include "netcache.h"

#include "config.h"
#include "log.h"
#include "lograte.h"

#include <iphlpapi.h>

#include <algorithm>
#include <string>
#include <vector>

#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT (0xA0000003L)
#endif

namespace {

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring ExpandPath(std::wstring p)
{
    if (p.find(L"%LOCALAPPDATA%") == 0) {
        wchar_t la[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH))
            p = std::wstring(la) + p.substr(14);
    }
    return p;
}

bool IsReparsePoint(const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* NTFS mount point (junction) via FSCTL_SET_REPARSE_POINT. On failure the
   empty directory created above is removed again (no litter). */
bool CreateJunction(const std::wstring& link, const std::wstring& target)
{
    bool createdDir = false;
    if (CreateDirectoryW(link.c_str(), nullptr))
        createdDir = true;
    else if (GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT |
                           FILE_FLAG_BACKUP_SEMANTICS,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (createdDir) RemoveDirectoryW(link.c_str());
        return false;
    }

    std::wstring sub = L"\\??\\" + target;

    #pragma pack(push, 4)
    struct MountBuf {
        DWORD ReparseTag;
        WORD  ReparseDataLength;
        WORD  Reserved;
        WORD  SubstituteNameOffset;
        WORD  SubstituteNameLength;
        WORD  PrintNameOffset;
        WORD  PrintNameLength;
        WCHAR PathBuffer[520];
    } buf = {};
    #pragma pack(pop)

    WORD subBytes = (WORD)(sub.size() * sizeof(wchar_t));
    buf.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    buf.SubstituteNameOffset = 0;
    buf.SubstituteNameLength = subBytes;
    buf.PrintNameOffset = subBytes + sizeof(WCHAR); /* keep the null slot */
    buf.PrintNameLength = 0;
    memcpy(buf.PathBuffer, sub.c_str(), subBytes);
    buf.ReparseDataLength = (WORD)(4 * sizeof(WORD) + subBytes + sizeof(WCHAR));

    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &buf,
                              sizeof(buf.ReparseTag) + buf.ReparseDataLength,
                              nullptr, 0, &ret, nullptr);
    CloseHandle(h);

    if (!ok) {
        LOGW("[Net] Junction create failed for %s | Code: %lu",
             Narrow(link).c_str(), (unsigned long)GetLastError());
        if (createdDir) RemoveDirectoryW(link.c_str());
    }
    return ok != FALSE;
}

void EnsureSharedCache(const std::wstring& linkIn, const std::wstring& rootIn)
{
    std::wstring link = ExpandPath(linkIn);
    std::wstring root = ExpandPath(rootIn);
    if (link.size() < 4 || root.size() < 4) return;

    std::wstring name = link.substr(link.find_last_of(L"\\/") + 1);
    std::wstring target = root + L"\\" + name;

    if (!CreateDirectoryW(root.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return;

    DWORD attrs = GetFileAttributesW(link.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (IsReparsePoint(link)) return;      /* already a link: done */
        /* Real dir: convert only if empty, otherwise recommend. */
        std::wstring probe = link + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW(probe.c_str(), &fd);
        bool empty = true;
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                    { empty = false; break; }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        if (empty) {
            if (!RemoveDirectoryW(link.c_str())) return;
            if (CreateJunction(link, target))
                LOGI("[Net] Cache junction created: %s", Narrow(link).c_str());
        }
        else {
            LOGW("[Net] NOTE: non-empty cache dir %s - merge its contents into %s manually, then rerun TASX.",
                 Narrow(link).c_str(), Narrow(target).c_str());
        }
        return;
    }

    if (CreateJunction(link, target))
        LOGI("[Net] Cache junction created: %s", Narrow(link).c_str());
}

} /* namespace */

void NetCacheApply()
{
    const char* root = config_get_str("Net", "SharedCacheRoot", "");
    if (!root[0]) return;
    std::wstring wroot(root, root + strlen(root));

    const char* links = config_get_str("Net", "CacheLinks", "");
    std::string s(links);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(';', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = s.substr(pos, end - pos);
        pos = end + 1;

        size_t b = item.find_first_not_of(" \t\"");
        size_t e = item.find_last_not_of(" \t\"");
        if (b == std::string::npos) continue;
        item = item.substr(b, e - b + 1);
        if (item.size() < 4) continue;

        int n = MultiByteToWideChar(CP_UTF8, 0, item.c_str(), -1, nullptr, 0);
        if (n <= 1) continue;
        std::wstring w(n - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, item.c_str(), -1, &w[0], n);
        EnsureSharedCache(w, wroot);
    }
}

void NetCacheLogConnections(const std::unordered_set<DWORD>& clientPids)
{
    if (clientPids.empty()) return;

    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER)
        return;

    std::vector<BYTE> buf(size);
    if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR)
        return;

    auto* table = (MIB_TCPTABLE_OWNER_PID*)buf.data();
    int established = 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i)
        if (table->table[i].dwState == MIB_TCP_STATE_ESTAB &&
            clientPids.count(table->table[i].dwOwningPid))
            ++established;

    LOGI("[Net] %u client(s), %d established connection(s)",
         (unsigned)clientPids.size(), established);
}
/* --- Shared-cache LRU trim ------------------------------------------- */

/* mtime (win FILETIME as u64), size. Files only - dirs and reparse points
   are never entered/collected, so junctions inside the cache root (which
   point at client installs!) can never be walked into or deleted. */
namespace {

struct CacheFile {
    std::wstring path;
    ULONGLONG   mtime;
    ULONGLONG   size;
};

void CollectCacheFiles(const std::wstring& dir, std::vector<CacheFile>& out,
                       int depth)
{
    if (depth > 4) return; /* extreme safety net - caches are shallow */

    /* lpFindFileData must point at writable storage: passing NULL makes the
       API write the first result to address 0 (access violation). The buffer
       is filled by FindFirstFileW and refreshed by every FindNextFileW. */
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue; /* junction/symlink: NEVER follow into client installs */

        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectCacheFiles(full, out, depth + 1);
        } else {
            ULONGLONG mtime = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) |
                              fd.ftLastWriteTime.dwLowDateTime;
            ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) |
                             fd.nFileSizeLow;
            out.push_back({full, mtime, size});
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

} /* namespace */

void NetCacheTrimIfOversized()
{
    const char* root = config_get_str("Net", "SharedCacheRoot", "");
    if (!root[0]) return;
    int maxGB = config_get_int("TASX", "CacheMaxGB", 0);
    if (maxGB <= 0) return;

    /* One scan per 10 min max - the walk is cheap for small caches but the
       daily farm cache can reach tens of GB with hundreds of thousands of
       small files. */
    if (!LogRateLimit("cache-trim-scan", 600)) return;

    std::wstring wroot(root, root + strlen(root));
    DWORD attrs = GetFileAttributesW(wroot.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
        return; /* missing/foreign root - never create or follow */

    std::vector<CacheFile> files;
    CollectCacheFiles(wroot, files, 0);
    if (files.empty()) return;

    ULONGLONG total = 0;
    for (const auto& f : files) total += f.size;

    ULONGLONG limitBytes = (ULONGLONG)maxGB << 30;
    if (total <= limitBytes) return;

    std::sort(files.begin(), files.end(),
              [](const CacheFile& a, const CacheFile& b) { return a.mtime < b.mtime; });

    ULONGLONG target = (ULONGLONG)((size_t)maxGB * 8 / 10) << 30; /* ~80% */
    ULONGLONG freed = 0;
    size_t deleted = 0;
    for (const auto& f : files) {
        if (total - freed <= target) break;
        if (DeleteFileW(f.path.c_str())) {
            freed += f.size;
            ++deleted;
        }
    }

    if (deleted) {
        double gbTotal = (double)total / (1ull << 30);
        double gbNow = (double)(total > freed ? total - freed : 0) / (1ull << 30);
        LOGI("[Net] Cache trim: %u file(s) deleted (%.2f GB), cache now %.2f/%.2f GB (limit %d GB)",
             (unsigned)deleted, (double)freed / (1ull << 30), gbNow, gbTotal, maxGB);
    } else {
        LOGW("[Net] Cache %.2f GB over %d GB limit but nothing deletable (all files locked/in use)",
             (double)total / (1ull << 30), maxGB);
    }
}
