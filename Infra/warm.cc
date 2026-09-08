#include "warm.h"

#include "config.h"

#include <algorithm>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_set>

namespace {

std::mutex g_primedPathsMtx;
std::unordered_set<std::wstring> g_primedPaths; // once per version dir

typedef BOOL (WINAPI *PrefetchVirtualMemoryFn)(HANDLE hProcess,
                                               ULONG_PTR numberOfEntries,
                                               void* virtualAddresses,
                                               ULONG flags);

struct RANGE {
    void*  start;
    SIZE_T bytes;
};

PrefetchVirtualMemoryFn GetPrefetchFn()
{
    static PrefetchVirtualMemoryFn fn = nullptr;
    static int resolved = 0;
    if (!resolved) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32)
            fn = (PrefetchVirtualMemoryFn)GetProcAddress(k32,
                                                         "PrefetchVirtualMemory");
        resolved = 1;
    }
    return fn; /* null on Win7 -> caller falls back to sequential reads */
}

/* Maps the file (shared, named file mapping of the on-disk image) and pulls
   its pages into the shared standby cache once. */
bool WarmFile(const std::wstring& path, ULONGLONG& budget)
{
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0) {
        CloseHandle(hFile);
        return false;
    }

    ULONGLONG bytes = (ULONGLONG)size.QuadPart;
    if (bytes > budget) { CloseHandle(hFile); return false; }

    HANDLE hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (hMap) {
        void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (view) {
            PrefetchVirtualMemoryFn fn = GetPrefetchFn();
            if (fn) {
                RANGE r{ view, (SIZE_T)bytes };
                fn(GetCurrentProcess(), 1, &r, 0);
            }
            else {
                /* Win7 fallback: sequential read warms the same cache. */
                char sink[1 << 20];
                DWORD got = 0;
                while (ReadFile(hFile, sink, sizeof(sink), &got, nullptr) && got)
                    ;
            }
            UnmapViewOfFile(view);
        }
        CloseHandle(hMap);
    }

    budget -= bytes;
    CloseHandle(hFile);
    return true;
}

int FilePriority(const wchar_t* name)
{
    const wchar_t* ext = wcsrchr(name, L'.');
    if (!ext) return 2;
    if (_wcsicmp(ext, L".exe") == 0 || _wcsicmp(ext, L".dll") == 0) return 0;
    if (_wcsicmp(ext, L".pak") == 0) return 1;
    return 2;
}

} /* namespace */

void WarmClientFilesAsync(DWORD pid)
{
    if (!config_get_bool("TASX", "WarmClientFiles", 1)) return;
    if (!config_get_bool("TASX", "WarmOncePerVersion", 1)) return; // already default

    // Cache primed paths globally — only prime once per version directory
    std::thread([pid]() {
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) return;

        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        BOOL ok = QueryFullProcessImageNameW(hProc, 0, path, &size);
        CloseHandle(hProc);
        if (!ok) return;

        std::wstring dir(path);
        size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return;
        dir.resize(slash);

    std::lock_guard<std::mutex> lock(g_primedPathsMtx); // protect cache set
    if (g_primedPaths.count(dir)) {
            std::cout << "[Warm] Version dir already primed: " << std::string(dir.begin(), dir.end()) << std::endl;
            return; // no-op for same version
        }
        dir.resize(slash);

        ULONGLONG budget =
            (ULONGLONG)std::max(0, config_get_int("TASX", "WarmMaxMB", 256)) << 20;
        if (budget < (1ull << 20)) return;

        /* exe/dll first (image pages shared by every instance), then pak. */
        struct File { std::wstring name; ULONGLONG bytes; int prio; };
        std::vector<File> files;

        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ULONGLONG lo = fd.nFileSizeLow, hi = fd.nFileSizeHigh;
            files.push_back({ fd.cFileName, lo | (hi << 32),
                              FilePriority(fd.cFileName) });
        } while (FindNextFileW(find, &fd));
        FindClose(find);

        std::sort(files.begin(), files.end(), [](const File& a, const File& b) {
            if (a.prio != b.prio) return a.prio < b.prio;
            return a.bytes > b.bytes;
        });

        ULONGLONG warmed = 0;
        for (const auto& f : files) {
            if (budget < (1ull << 20)) break;
            if (WarmFile(dir + L"\\" + f.name, budget))
                warmed += f.bytes;
        }

        if (warmed) {
            std::lock_guard<std::mutex> lock(g_primedPathsMtx);
            g_primedPaths.insert(dir);
            std::cout << "[Warm] Shared pages primed (once): " << (warmed >> 20)
                      << " MB from " << std::string(dir.begin(), dir.end())
                      << std::endl;
        }
    }).detach();
}
