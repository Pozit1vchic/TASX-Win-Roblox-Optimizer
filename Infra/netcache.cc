#include "netcache.h"

#include "config.h"

#include <iphlpapi.h>

#include <iostream>
#include <string>
#include <vector>

#ifndef IO_REPARSE_TAG_MOUNT_POINT
#define IO_REPARSE_TAG_MOUNT_POINT (0xA0000003L)
#endif

namespace {

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

/* NTFS mount point (junction) via FSCTL_SET_REPARSE_POINT. */
bool CreateJunction(const std::wstring& link, const std::wstring& target)
{
    if (!CreateDirectoryW(link.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT |
                           FILE_FLAG_BACKUP_SEMANTICS,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

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

    if (!ok)
        std::cout << "[Net] Junction create failed for "
                  << std::string(link.begin(), link.end())
                  << " | Code: " << GetLastError() << std::endl;
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
                std::cout << "[Net] Cache junction created: "
                          << std::string(link.begin(), link.end()) << std::endl;
        }
        else {
            std::cout << "[Net] NOTE: non-empty cache dir "
                      << std::string(link.begin(), link.end())
                      << " - merge its contents into "
                      << std::string(target.begin(), target.end())
                      << " manually, then rerun TASX." << std::endl;
        }
        return;
    }

    if (CreateJunction(link, target))
        std::cout << "[Net] Cache junction created: "
                  << std::string(link.begin(), link.end()) << std::endl;
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

    std::cout << "[Net] " << clientPids.size() << " client(s), "
              << established << " established connection(s)" << std::endl;
}
