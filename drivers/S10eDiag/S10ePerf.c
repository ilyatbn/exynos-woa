/*
 * s10eperf.exe - quick, self-contained throughput numbers (notes/m15-perf.md). winsat prints only its banner
 * when run as SYSTEM from the agent (run 21), so this measures what matters for "everything is slow":
 *   cpu  - per logical CPU: a chain of dependent 64-bit XOR/ADD ops (one per cycle on these cores' integer
 *          pipes, so ops/ns ~= effective GHz; an estimate, not a cycle counter), thread pinned to that CPU
 *   mem  - memcpy bandwidth over 64 MiB
 *   disk - sequential write/read of a 256 MiB unbuffered file, then 4 KiB random reads for 5 s
 *
 *   s10eperf [cpu] [mem] [disk <dir>] [raw <vol>] [sweep <vol>] [loop <vol> <s>]   (no arguments = cpu mem)
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double
Now(void)
{
    LARGE_INTEGER F, C;
    QueryPerformanceFrequency(&F);
    QueryPerformanceCounter(&C);
    return (double)C.QuadPart / (double)F.QuadPart;
}

static unsigned long long volatile gSink;

static double
AddChain(unsigned long long Iterations)
{
    unsigned long long A = 1, B = 3, I;
    double T0 = Now(), T1;

    for (I = 0; I < Iterations; I++) {
        // 8 dependent single-cycle ops per iteration, alternating XOR and ADD so the compiler cannot
        // reassociate/fold them (plain adds could legally collapse into one multiply-add).
        A ^= B; A += I; A ^= B; A += I; A ^= B; A += I; A ^= B; A += I;
    }
    T1 = Now();
    gSink = A;
    return (double)(Iterations * 8ULL) / ((T1 - T0) * 1e9);   // adds per ns ~= GHz
}

static void
Cpu(void)
{
    SYSTEM_INFO Info;
    DWORD Index;

    GetSystemInfo(&Info);
    printf("cpu: %lu logical processors\n", Info.dwNumberOfProcessors);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (Index = 0; Index < Info.dwNumberOfProcessors; Index++) {
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << Index);
        Sleep(10);
        AddChain(20000000ULL);   // warm-up / let the core ramp
        printf("cpu%lu: ~%.2f GHz (dependent adds)\n", Index, AddChain(100000000ULL));
    }
    SetThreadAffinityMask(GetCurrentThread(), ~(DWORD_PTR)0);
}

static void
Mem(void)
{
    size_t Size = 64u << 20;
    char *A = (char *)VirtualAlloc(NULL, Size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    char *B = (char *)VirtualAlloc(NULL, Size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    double T0, T1;
    int Pass;

    if (A == NULL || B == NULL) {
        printf("mem: alloc failed\n");
        return;
    }
    memset(A, 1, Size);
    memset(B, 2, Size);
    T0 = Now();
    for (Pass = 0; Pass < 8; Pass++) {
        memcpy(B, A, Size);
    }
    T1 = Now();
    printf("mem: memcpy %.0f MB/s\n", (double)Size * 8.0 / (T1 - T0) / 1e6);
    VirtualFree(A, 0, MEM_RELEASE);
    VirtualFree(B, 0, MEM_RELEASE);
}

static void
Disk(const char *Dir)
{
    char Path[MAX_PATH];
    const DWORD Chunk = 1u << 20;
    const DWORD Chunks = 256;
    BYTE *Buffer = (BYTE *)VirtualAlloc(NULL, Chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE File;
    DWORD Done, I;
    double T0, T1;
    unsigned Ops = 0;

    if (Buffer == NULL) {
        return;
    }
    memset(Buffer, 0x5A, Chunk);
    sprintf_s(Path, sizeof(Path), "%s\\s10eperf.tmp", Dir);

    File = CreateFileA(Path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
    if (File == INVALID_HANDLE_VALUE) {
        printf("disk: create %s failed %lu\n", Path, GetLastError());
        return;
    }
    T0 = Now();
    for (I = 0; I < Chunks; I++) {
        if (!WriteFile(File, Buffer, Chunk, &Done, NULL)) {
            printf("disk: write failed %lu\n", GetLastError());
            break;
        }
    }
    T1 = Now();
    CloseHandle(File);
    printf("disk: seq write %.1f MB/s (%lu MiB, unbuffered write-through)\n", (double)I * Chunk / (T1 - T0) / 1e6, I);

    File = CreateFileA(Path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (File == INVALID_HANDLE_VALUE) {
        printf("disk: open failed %lu\n", GetLastError());
        return;
    }
    T0 = Now();
    for (I = 0; I < Chunks; I++) {
        if (!ReadFile(File, Buffer, Chunk, &Done, NULL) || Done == 0) {
            break;
        }
    }
    T1 = Now();
    printf("disk: seq read %.1f MB/s\n", (double)I * Chunk / (T1 - T0) / 1e6);

    srand(12345);
    T0 = Now();
    while (Now() - T0 < 5.0) {
        LARGE_INTEGER Offset;
        Offset.QuadPart = (LONGLONG)(rand() % (Chunks * 256)) * 4096;
        SetFilePointerEx(File, Offset, NULL, FILE_BEGIN);
        if (!ReadFile(File, Buffer, 4096, &Done, NULL)) {
            break;
        }
        Ops++;
    }
    T1 = Now();
    printf("disk: random 4K read %.0f IOPS (%.1f MB/s), queue depth 1\n", Ops / (T1 - T0), Ops * 4096.0 / (T1 - T0) / 1e6);
    CloseHandle(File);
    DeleteFileA(Path);
    VirtualFree(Buffer, 0, MEM_RELEASE);
}

//
// Raw volume read (read-only): \\.\C: in 4 KiB / 64 KiB / 1 MiB unbuffered requests from offset 0 - bypasses NTFS layout and
// fragmentation, so it shows what the storage stack itself delivers (m15 run 28: file reads ~23 MB/s).
//
static void
RawRead(const char *Volume)
{
    const DWORD Chunk = 1u << 20;
    BYTE *Buffer = (BYTE *)VirtualAlloc(NULL, Chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE File = CreateFileA(Volume, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_FLAG_NO_BUFFERING, NULL);
    DWORD Done, I, Size;
    double T0, T1;

    if (Buffer == NULL || File == INVALID_HANDLE_VALUE) {
        printf("raw: open %s failed %lu\n", Volume, GetLastError());
        return;
    }
    for (Size = 4096; Size <= Chunk; Size *= 16) {
        DWORD Count = (Size == Chunk) ? 256 : 2048;
        LARGE_INTEGER Zero;

        Zero.QuadPart = 0;
        SetFilePointerEx(File, Zero, NULL, FILE_BEGIN);
        T0 = Now();
        for (I = 0; I < Count; I++) {
            if (!ReadFile(File, Buffer, Size, &Done, NULL) || Done != Size) {
                break;
            }
        }
        T1 = Now();
        printf("raw: %s %6lu-byte reads: %.1f MB/s, %.0f us per request\n", Volume, Size,
               (double)I * Size / (T1 - T0) / 1e6, (T1 - T0) * 1e6 / (I ? I : 1));
    }
    CloseHandle(File);
    VirtualFree(Buffer, 0, MEM_RELEASE);
}

//
// m15 UFS size sweep (read-only, notes/m15-ufs-research-dma.md suspect 2): where does the per-request cost stop
// being Linux-class? 512 unbuffered reads per size, 4..64 KiB in 4 KiB steps.
//
static void
RawSweep(const char *Volume)
{
    const DWORD Max = 64u << 10;
    BYTE *Buffer = (BYTE *)VirtualAlloc(NULL, Max, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE File = CreateFileA(Volume, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_FLAG_NO_BUFFERING, NULL);
    DWORD Done, I, Size;
    double T0, T1;

    if (Buffer == NULL || File == INVALID_HANDLE_VALUE) {
        printf("sweep: open %s failed %lu\n", Volume, GetLastError());
        return;
    }
    for (Size = 4096; Size <= Max; Size += 4096) {
        LARGE_INTEGER Zero;

        Zero.QuadPart = 0;
        SetFilePointerEx(File, Zero, NULL, FILE_BEGIN);
        T0 = Now();
        for (I = 0; I < 512; I++) {
            if (!ReadFile(File, Buffer, Size, &Done, NULL) || Done != Size) {
                break;
            }
        }
        T1 = Now();
        printf("sweep: %6lu-byte reads: %.1f MB/s, %.0f us per request (%lu ok)\n", Size,
               (double)I * Size / (T1 - T0) / 1e6, (T1 - T0) * 1e6 / (I ? I : 1), I);
    }
    CloseHandle(File);
    VirtualFree(Buffer, 0, MEM_RELEASE);
}

//
// Background load for register sampling (read-only): 64 KiB unbuffered reads for Seconds, wrapping at 256 MiB.
//
static void
RawLoop(const char *Volume, int Seconds)
{
    const DWORD Size = 64u << 10;
    BYTE *Buffer = (BYTE *)VirtualAlloc(NULL, Size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    HANDLE File = CreateFileA(Volume, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_FLAG_NO_BUFFERING, NULL);
    DWORD Done, Count = 0;
    double T0;

    if (Buffer == NULL || File == INVALID_HANDLE_VALUE) {
        printf("loop: open %s failed %lu\n", Volume, GetLastError());
        return;
    }
    T0 = Now();
    while (Now() - T0 < Seconds) {
        if ((Count & 4095) == 0) {
            LARGE_INTEGER Zero;

            Zero.QuadPart = 0;
            SetFilePointerEx(File, Zero, NULL, FILE_BEGIN);
        }
        if (!ReadFile(File, Buffer, Size, &Done, NULL) || Done != Size) {
            break;
        }
        Count++;
    }
    printf("loop: %lu x 64 KiB in %.1f s\n", Count, Now() - T0);
    CloseHandle(File);
    VirtualFree(Buffer, 0, MEM_RELEASE);
}

int
main(int argc, char **argv)
{
    int I;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        Cpu();
        Mem();
        return 0;
    }
    for (I = 1; I < argc; I++) {
        if (strcmp(argv[I], "cpu") == 0) {
            Cpu();
        } else if (strcmp(argv[I], "mem") == 0) {
            Mem();
        } else if (strcmp(argv[I], "raw") == 0 && I + 1 < argc) {
            RawRead(argv[++I]);
        } else if (strcmp(argv[I], "sweep") == 0 && I + 1 < argc) {
            RawSweep(argv[++I]);
        } else if (strcmp(argv[I], "loop") == 0 && I + 2 < argc) {
            RawLoop(argv[I + 1], atoi(argv[I + 2]));
            I += 2;
        } else if (strcmp(argv[I], "disk") == 0 && I + 1 < argc) {
            Disk(argv[++I]);
        }
    }
    return 0;
}
