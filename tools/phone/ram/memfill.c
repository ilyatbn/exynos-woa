/*
 * memfill - RAM stress for R1 (notes/ram-6gb.md): commit MB megabytes in 64 MB chunks (VirtualAlloc), write an
 * address-dependent pattern to every 8-byte word, read it all back PASSES times and count mismatches; prints the
 * physical-memory load before/after. Filling most of the free memory forces Windows onto every physical page,
 * including the newly published ranges - a page the secure world still owns would fault or read back wrong.
 * usage: memfill MB [PASSES]. Exit 0 = no mismatch. SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static void load(const char *tag)
{
    MEMORYSTATUSEX m = { sizeof(m) };
    GlobalMemoryStatusEx(&m);
    printf("%s: load %lu%%, avail phys %llu MB of %llu MB\n", tag, m.dwMemoryLoad, m.ullAvailPhys >> 20, m.ullTotalPhys >> 20);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: memfill MB [PASSES]\n"); return 1; }
    size_t mb = (size_t)atoi(argv[1]);
    int passes = argc > 2 ? atoi(argv[2]) : 2;
    const size_t chunk = 64u << 20;
    size_t n = (mb << 20) / chunk;
    unsigned long long **c = (unsigned long long **)calloc(n, sizeof(*c));
    load("start");
    size_t got = 0;
    for (size_t i = 0; i < n; i++) {
        c[i] = (unsigned long long *)VirtualAlloc(NULL, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!c[i]) { printf("alloc stopped at %zu MB (err %lu)\n", got >> 20, GetLastError()); break; }
        size_t w = chunk / 8;
        for (size_t k = 0; k < w; k++)
            c[i][k] = ((unsigned long long)(uintptr_t)&c[i][k]) ^ 0xA5A5A5A55A5A5A5AULL;
        got += chunk;
    }
    printf("written %zu MB\n", got >> 20);
    load("filled");
    unsigned long long bad = 0;
    for (int p = 0; p < passes; p++) {
        for (size_t i = 0; i < n && c[i]; i++) {
            size_t w = chunk / 8;
            for (size_t k = 0; k < w; k++)
                if (c[i][k] != (((unsigned long long)(uintptr_t)&c[i][k]) ^ 0xA5A5A5A55A5A5A5AULL)) {
                    if (bad < 8) printf("MISMATCH chunk %zu word %zu: %016llx\n", i, k, c[i][k]);
                    bad++;
                }
        }
        printf("pass %d: %llu mismatches\n", p + 1, bad);
    }
    for (size_t i = 0; i < n && c[i]; i++) VirtualFree(c[i], 0, MEM_RELEASE);
    load("end");
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 3 : 0;
}
