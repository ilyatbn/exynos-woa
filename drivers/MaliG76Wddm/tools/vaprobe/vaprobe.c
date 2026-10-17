/*
 * vaprobe.c - G-game (notes/g17-dxva-mfc.md): where a process's virtual address space goes. Walks it with
 * VirtualQueryEx and sums reserved / committed bytes by type (image / mapped / private), then lists the largest
 * allocation groups (regions sharing an AllocationBase) with their mapped file or image name. A 32-bit game ran out
 * of its 4 GB address space while its working set stayed ~250 MB.
 *
 *   vaprobe PID [top=25]
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "psapi.lib")

typedef struct { ULONG_PTR base; SIZE_T reserved, committed; DWORD type; } GROUP;

static int cmp(const void *a, const void *b)
{
    SIZE_T x = ((const GROUP *)a)->reserved, y = ((const GROUP *)b)->reserved;
    return x < y ? 1 : x > y ? -1 : 0;
}

int main(int argc, char **argv)
{
    DWORD pid = argc > 1 ? (DWORD)atoi(argv[1]) : 0;
    int top = argc > 2 ? atoi(argv[2]) : 25;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        printf("OpenProcess %lu failed (%lu)\n", pid, GetLastError());
        return 1;
    }
    static GROUP g[65536];
    int ng = 0;
    SIZE_T tot[3][2] = { 0 };   /* image / mapped / private x reserved-only / committed */
    MEMORY_BASIC_INFORMATION m;
    ULONG_PTR p = 0;
    while (VirtualQueryEx(h, (LPCVOID)p, &m, sizeof(m)) == sizeof(m)) {
        if (m.State != MEM_FREE) {
            int t = m.Type == MEM_IMAGE ? 0 : m.Type == MEM_MAPPED ? 1 : 2;
            tot[t][m.State == MEM_COMMIT] += m.RegionSize;
            ULONG_PTR ab = (ULONG_PTR)m.AllocationBase;
            if (ng == 0 || g[ng - 1].base != ab) {
                if (ng < 65536) {
                    g[ng].base = ab; g[ng].reserved = 0; g[ng].committed = 0; g[ng].type = m.Type;
                    ng++;
                }
            }
            g[ng - 1].reserved += m.RegionSize;
            if (m.State == MEM_COMMIT)
                g[ng - 1].committed += m.RegionSize;
        }
        ULONG_PTR next = (ULONG_PTR)m.BaseAddress + m.RegionSize;
        if (next <= p)
            break;
        p = next;
    }
    const char *tn[3] = { "image", "mapped", "private" };
    for (int t = 0; t < 3; t++)
        printf("%-8s reserved-only %8.1f MB, committed %8.1f MB\n", tn[t], tot[t][0] / 1048576.0, tot[t][1] / 1048576.0);
    qsort(g, ng, sizeof(g[0]), cmp);
    printf("%d allocation groups; largest:\n", ng);
    for (int i = 0; i < ng && i < top; i++) {
        char name[MAX_PATH] = "";
        if (g[i].type != MEM_PRIVATE)
            GetMappedFileNameA(h, (LPVOID)g[i].base, name, sizeof(name));
        printf("  0x%016llx %8.1f MB (committed %8.1f) %-7s %s\n", (unsigned long long)g[i].base,
               g[i].reserved / 1048576.0, g[i].committed / 1048576.0,
               g[i].type == MEM_IMAGE ? "image" : g[i].type == MEM_MAPPED ? "mapped" : "private", name);
    }
    CloseHandle(h);
    return 0;
}
