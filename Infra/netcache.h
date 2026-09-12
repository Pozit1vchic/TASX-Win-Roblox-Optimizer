#pragma once

#include <windows.h>
#include <unordered_set>

/* Network density layer (injection-free).

   Connect-hooks / socket sharing inside the client are impossible here:
   game sockets are per-account authenticated, and hooking Winsock means
   DLL injection -> Hyperion detection. What actually pays off:

   1) Cache sharing. Multiple client copies download the same CDN assets
      into separate per-copy cache folders. NTFS junctions make every copy
      resolve to ONE shared cache dir -> downloads and disk bytes dedup.
      Empty dirs are converted automatically; non-empty ones only get a
      recommendation (no data is moved automatically).
   2) Connection observability: one GetExtendedTcpTable call counts live
      client connections so density problems are visible. */
void NetCacheApply();

void NetCacheLogConnections(const std::unordered_set<DWORD>& clientPids);

/* LRU prune of the TASX-managed shared cache ([Net] SharedCacheRoot):
   when the cache exceeds [TASX] CacheMaxGB, the oldest files are deleted
   until ~80% of the limit. Only regular files inside the dedicated cache
   root are touched - junctions/reparse points are never followed, so the
   client installs themselves are completely safe. No-op without a
   configured SharedCacheRoot or CacheMaxGB=0. Rate-limited internally to
   one scan per 10 minutes. */
void NetCacheTrimIfOversized(void);
