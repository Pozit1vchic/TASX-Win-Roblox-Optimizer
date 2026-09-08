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

bool QueryCoreLoads(std::vector<double>& busyPct)
{
    static std::vector<double> prevIdle, prevBusy;
    static bool havePrev = false;

    unsigned long count = 0;
    TASX_SYS_PROC_PERF* perf = tasx_query_processor_performance(&count);
    if (!perf || count == 0) return false;

    std::vector<double> idle(count), busy(count);
    for (unsigned long i = 0; i < count; ++i) {
        idle[i] = (double)perf[i].IdleTime.QuadPart;
        busy[i] = (double)perf[i].KernelTime.QuadPart
                + (double)perf[i].UserTime.QuadPart;
    }
    free(perf);

    if (!havePrev) {
        prevIdle = std::move(idle);
        prevBusy = std::move(busy);
        havePrev = true;
        return false; /* need a delta window */
    }

    busyPct.assign(count, 0.0);
    for (unsigned long i = 0; i < count; ++i) {
        double db = busy[i] - prevBusy[i];
        double di = idle[i] - prevIdle[i];
        double total = db + di;
        busyPct[i] = total > 0.0 ? db / total * 100.0 : 0.0;
    }
    prevIdle = std::move(idle);
    prevBusy = std::move(busy);
    return true;
}
