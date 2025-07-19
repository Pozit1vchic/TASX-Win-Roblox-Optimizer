#include <windows.h>

#pragma comment(lib, "PowrProf.lib")

#ifndef ProcessorPerformanceBoostMode
#define ProcessorPerformanceBoostMode ((POWER_INFORMATION_LEVEL)35)
#endif

bool TasxSetLowestPriorClass(HANDLE hProcess);
bool TasxSetHighestPriorClass(HANDLE hProcess);