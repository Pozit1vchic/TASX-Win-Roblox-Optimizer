#include "stats.h"

#include "ntsys.h"

bool QueryProcStats(std::vector<ProcStat>& out)
{
    TASX_SYS_PROC* head = tasx_query_system_processes();
    if (!head) return false;

    /* Reserve generously once; typical systems have a few hundred entries. */
    if (out.capacity() < 256) out.reserve(256);
    out.clear();

    TASX_SYS_PROC* p = head;
    for (;;)
    {
        if (p->UniqueProcessId)
        {
            ProcStat st;
            st.pid = (DWORD)(ULONG_PTR)p->UniqueProcessId;
            st.workingSet = p->WorkingSetSize;
            st.privateBytes = p->PrivatePageCount;
            if (p->ImageName.Buffer && p->ImageName.Length &&
                p->ImageName.Length < 512) /* sanity: layout guard */
                st.name.assign(p->ImageName.Buffer,
                               p->ImageName.Length / sizeof(wchar_t));
            out.push_back(std::move(st));
        }

        if (!p->NextEntryOffset) break;
        p = (TASX_SYS_PROC*)((unsigned char*)p + p->NextEntryOffset);
    }

    free(head);
    return true;
}

std::wstring QueryProcessNameByPid(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return std::wstring();

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &size))
        name = path;
    CloseHandle(h);
    return name;
}
