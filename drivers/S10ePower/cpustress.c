/*
 * cpustress - CPU stability check for clock changes (notes/clocks-dvfs.md, user 2026-10-17: "run a stability check").
 * One thread pinned to each logical processor repeats a deterministic workload - integer hashing, a double-precision
 * matrix multiply and a 1 MB memory copy + checksum - and compares every result with the reference that thread
 * computed first. A mismatch is a silent computation error (an unstable clock / voltage), the failure a plain "did
 * not crash" test misses. Progress every 10 s: iterations and errors per core.
 *
 *   cpustress [seconds=60] [cpumask=0x3f]
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 48
typedef struct { int cpu; volatile LONG iters, errors; volatile LONG stop; } WORKER;

static unsigned long long Work(unsigned seed, unsigned char *buf, unsigned char *buf2)
{
    unsigned long long h = 0x9E3779B97F4A7C15ull ^ seed;
    for (int i = 0; i < 200000; i++) {                       /* integer: multiply / xor / rotate */
        h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32; h += (unsigned)i;
    }
    double a[N][N], b[N][N], c[N][N];                        /* FP: matrix multiply */
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            a[i][j] = (double)((i * 31 + j * 17 + seed) % 97) / 7.0;
            b[i][j] = (double)((i * 13 + j * 29 + seed) % 89) / 5.0;
        }
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            double s = 0;
            for (int k = 0; k < N; k++)
                s += a[i][k] * b[k][j];
            c[i][j] = s;
        }
    unsigned long long fp = 0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            unsigned long long bits;
            memcpy(&bits, &c[i][j], 8);
            fp = fp * 1099511628211ull ^ bits;
        }
    for (int i = 0; i < (1 << 20); i++)                      /* memory: fill, copy, checksum */
        buf[i] = (unsigned char)(i * 7 + seed);
    memcpy(buf2, buf, 1 << 20);
    unsigned long long m = 0;
    for (int i = 0; i < (1 << 20); i += 8) {
        unsigned long long v;
        memcpy(&v, buf2 + i, 8);
        m = (m ^ v) * 0x100000001B3ull;
    }
    return h ^ (fp * 3) ^ (m * 5);
}

static DWORD WINAPI Worker(LPVOID p)
{
    WORKER *w = (WORKER *)p;
    SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << w->cpu);
    unsigned char *buf = (unsigned char *)malloc(1 << 20), *buf2 = (unsigned char *)malloc(1 << 20);
    if (!buf || !buf2)
        return 1;
    unsigned seed = 1234u + (unsigned)w->cpu;
    unsigned long long ref = Work(seed, buf, buf2);
    while (!w->stop) {
        if (Work(seed, buf, buf2) != ref)
            InterlockedIncrement(&w->errors);
        InterlockedIncrement(&w->iters);
    }
    free(buf);
    free(buf2);
    return 0;
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    unsigned mask = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 0x3f;
    WORKER w[32] = { 0 };
    HANDLE t[32];
    int n = 0;
    for (int cpu = 0; cpu < 32; cpu++)
        if (mask & (1u << cpu)) {
            w[n].cpu = cpu;
            t[n] = CreateThread(NULL, 0, Worker, &w[n], 0, NULL);
            n++;
        }
    printf("cpustress: %d threads, mask 0x%x, %d s\n", n, mask, secs);
    for (int s = 10; s <= secs || (s - 10) < secs; s += 10) {
        Sleep((DWORD)(s > secs ? (secs - (s - 10)) : 10) * 1000);
        printf("t=%3d s:", s > secs ? secs : s);
        for (int i = 0; i < n; i++)
            printf(" cpu%d %ld it %ld err |", w[i].cpu, w[i].iters, w[i].errors);
        printf("\n");
        fflush(stdout);
        if (s >= secs)
            break;
    }
    LONG errs = 0;
    for (int i = 0; i < n; i++) {
        w[i].stop = 1;
        errs += w[i].errors;
    }
    WaitForMultipleObjects((DWORD)n, t, TRUE, 30000);
    printf("%s: %ld errors\n", errs ? "FAIL" : "PASS", errs);
    return errs ? 2 : 0;
}
