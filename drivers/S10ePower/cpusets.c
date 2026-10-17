/*
 * cpusets - what the Windows scheduler knows about each logical CPU (GetSystemCpuSetInformation): id, group/LP index,
 * core / last-level cache / NUMA index, EfficiencyClass (higher = faster; our MADT: A55 0, A75 1, M4 2), parked,
 * allocated, realtime. Read-only (notes/clocks-dvfs.md C4).
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    ULONG len = 0;
    BYTE *buf, *p;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    buf = (BYTE *)malloc(len);
    if (!buf || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        printf("GetSystemCpuSetInformation failed %lu\n", GetLastError());
        return 1;
    }
    for (p = buf; p < buf + len; p += ((PSYSTEM_CPU_SET_INFORMATION)p)->Size) {
        PSYSTEM_CPU_SET_INFORMATION s = (PSYSTEM_CPU_SET_INFORMATION)p;
        if (s->Type != CpuSetInformation)
            continue;
        printf("cpuset %lu lp %u core %u llc %u numa %u efficiency %u parked %u allocated %u realtime %u\n",
               s->CpuSet.Id, s->CpuSet.LogicalProcessorIndex, s->CpuSet.CoreIndex, s->CpuSet.LastLevelCacheIndex,
               s->CpuSet.NumaNodeIndex, s->CpuSet.EfficiencyClass, s->CpuSet.Parked, s->CpuSet.Allocated,
               s->CpuSet.RealTime);
    }
    return 0;
}
