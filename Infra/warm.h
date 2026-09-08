#pragma once

#include <windows.h>

/* Shared-page warmer for instance density.

   The kernel already deduplicates file-backed (read-only) pages: every
   client of the same version maps the SAME physical pages of
   RobloxPlayerBeta.exe / its DLLs / .pak assets. What kills density is
   those shared pages NOT being resident (each client then soft-faults and
   re-reads them individually).

   TASX maps each client's install files (named file mappings of the very
   files the clients execute from) and prefetches them once into the shared
   standby cache -> one warm pass serves all 100+ instances. No injection,
   no client memory is touched. Runs on a detached thread per hook. */
void WarmClientFilesAsync(DWORD pid);
