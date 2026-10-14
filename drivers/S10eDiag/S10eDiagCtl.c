/*
 * s10ediag.exe - command line for S10eDiag.sys. notes/m11-s10ediag.md.
 *
 *   s10ediag info
 *   s10ediag peek <phys> [bytes]        MMIO, 32-bit reads, hex dump
 *   s10ediag dumpram <phys> <bytes> <file>
 *   s10ediag dumpmmio <phys> <bytes> <file>    as dumpram, through an uncached mapping (non-RAM, e.g. the framebuffer)
 *   s10ediag poke <phys> <value>
 *   s10ediag recovery                   next reset (clean `shutdown /r`) goes to TWRP
 *   s10ediag hardreset                  INFORM3 = recovery + SWRESET now
 *   s10ediag fallback <seconds>         hard reset to TWRP after <seconds> (0 = cancel)
 *   s10ediag mark <text...>             one line in the PRAM mark ring (survives resets)
 */

#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include "S10eDiagIoctl.h"

static HANDLE
OpenDiag(void)
{
    HANDLE Handle = CreateFileW(S10EDIAG_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    if (Handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "open %ls failed: %lu\n", S10EDIAG_USER_PATH, GetLastError());
        exit(2);
    }
    return Handle;
}

static int
Ioctl(DWORD Code, void *In, DWORD InLength, void *Out, DWORD OutLength, DWORD *Returned)
{
    DWORD Dummy = 0;
    HANDLE Handle = OpenDiag();
    BOOL Ok = DeviceIoControl(Handle, Code, In, InLength, Out, OutLength, Returned ? Returned : &Dummy, NULL);

    if (!Ok) {
        fprintf(stderr, "ioctl 0x%08lx failed: %lu\n", Code, GetLastError());
    }
    CloseHandle(Handle);
    return Ok ? 0 : 1;
}

static int
Peek(unsigned long long Address, unsigned long Length)
{
    S10EDIAG_READ_IN In = { Address, Length, S10EDIAG_READ_MMIO };
    unsigned long *Buffer = (unsigned long *)calloc(1, Length < sizeof(In) ? sizeof(In) : Length);
    DWORD Returned = 0;
    unsigned long Index;

    memcpy(Buffer, &In, sizeof(In));
    if (Ioctl(IOCTL_S10EDIAG_READ_PHYS, Buffer, sizeof(In), Buffer, Length, &Returned) != 0) {
        return 1;
    }
    for (Index = 0; Index < Returned / 4; Index++) {
        if ((Index % 4) == 0) {
            printf("%s%010llx:", Index ? "\n" : "", Address + Index * 4ULL);
        }
        printf(" %08lx", Buffer[Index]);
    }
    printf("\n");
    {
        // The first words also go to the PRAM mark ring: a later read that aborts loses FAT writes.
        char Line[S10EDIAG_MARK_LINE];
        int Used = _snprintf_s(Line, sizeof(Line), _TRUNCATE, "peek %llx =", Address);

        for (Index = 0; Index < Returned / 4 && Index < 4 && Used > 0; Index++) {
            Used += _snprintf_s(Line + Used, sizeof(Line) - Used, _TRUNCATE, " %08lx", Buffer[Index]);
        }
        if (Used > 0) {
            Ioctl(IOCTL_S10EDIAG_MARK, Line, (DWORD)Used, NULL, 0, NULL);
        }
    }
    free(Buffer);
    return 0;
}

static int
DumpRam(unsigned long long Address, unsigned long Length, const char *Path, unsigned Mode)
{
    FILE *File = fopen(Path, "wb");
    unsigned char *Buffer = (unsigned char *)malloc(S10EDIAG_READ_MAX);
    unsigned long Done = 0;

    if ((File == NULL) || (Buffer == NULL)) {
        fprintf(stderr, "cannot open %s\n", Path);
        return 1;
    }
    while (Done < Length) {
        unsigned long Chunk = (Length - Done > S10EDIAG_READ_MAX) ? S10EDIAG_READ_MAX : Length - Done;
        S10EDIAG_READ_IN In = { Address + Done, Chunk, Mode };
        DWORD Returned = 0;

        memcpy(Buffer, &In, sizeof(In));
        if (Ioctl(IOCTL_S10EDIAG_READ_PHYS, Buffer, sizeof(In), Buffer, Chunk, &Returned) != 0) {
            break;
        }
        fwrite(Buffer, 1, Returned, File);
        Done += Chunk;
    }
    fclose(File);
    free(Buffer);
    printf("%lu bytes -> %s\n", Done, Path);
    return Done == Length ? 0 : 1;
}

int
main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "info") == 0) {
        S10EDIAG_INFO Info = { 0 };
        if (Ioctl(IOCTL_S10EDIAG_INFO, NULL, 0, &Info, sizeof(Info), NULL) != 0) {
            return 1;
        }
        printf("version %lu recovery_armed %lu fallback_s %lu inform3 0x%08lx uptime_ms %llu\n",
               Info.Version, Info.RecoveryArmed, Info.FallbackSeconds, Info.Inform3, Info.UptimeMs);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "peek") == 0) {
        return Peek(_strtoui64(argv[2], NULL, 0), argc >= 4 ? strtoul(argv[3], NULL, 0) : 4);
    }
    if (argc >= 5 && strcmp(argv[1], "dumpram") == 0) {
        return DumpRam(_strtoui64(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), argv[4], S10EDIAG_READ_RAM);
    }
    if (argc >= 5 && strcmp(argv[1], "dumpmmio") == 0) {
        return DumpRam(_strtoui64(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), argv[4], S10EDIAG_READ_MMIO);
    }
    if (argc >= 4 && strcmp(argv[1], "poke") == 0) {
        S10EDIAG_WRITE32_IN In = { _strtoui64(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), 0 };
        return Ioctl(IOCTL_S10EDIAG_WRITE32, &In, sizeof(In), NULL, 0, NULL);
    }
    if (argc >= 3 && strcmp(argv[1], "mark") == 0) {
        char Text[S10EDIAG_MARK_LINE];
        size_t Used = 0;
        int Arg;

        for (Arg = 2; Arg < argc && Used < sizeof(Text) - 1; Arg++) {
            size_t Length = strlen(argv[Arg]);
            if (Used != 0) {
                Text[Used++] = ' ';
            }
            if (Length > sizeof(Text) - 1 - Used) {
                Length = sizeof(Text) - 1 - Used;
            }
            memcpy(Text + Used, argv[Arg], Length);
            Used += Length;
        }
        return Ioctl(IOCTL_S10EDIAG_MARK, Text, (DWORD)Used, NULL, 0, NULL);
    }
    if (argc >= 2 && strcmp(argv[1], "recovery") == 0) {
        return Ioctl(IOCTL_S10EDIAG_RECOVERY_ON_RESET, NULL, 0, NULL, 0, NULL);
    }
    if (argc >= 2 && (strcmp(argv[1], "flush") == 0 || strcmp(argv[1], "flushreset") == 0)) {
        // m14 run 20: the clean restart does not reach TWRP (sboot boots UEFI, which continues into Windows).
        // Flush every lettered volume's cache to disk, then (flushreset) the proven hard reset to recovery.
        wchar_t Root[] = L"\\\\.\\A:";
        DWORD Drives = GetLogicalDrives();
        int Letter;

        for (Letter = 0; Letter < 26; Letter++) {
            HANDLE Volume;

            if ((Drives & (1UL << Letter)) == 0) {
                continue;
            }
            Root[4] = (wchar_t)(L'A' + Letter);
            Volume = CreateFileW(Root, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, 0, NULL);
            if (Volume != INVALID_HANDLE_VALUE) {
                printf("flush %c: %s\n", 'A' + Letter, FlushFileBuffers(Volume) ? "ok" : "failed");
                CloseHandle(Volume);
            }
        }
        if (strcmp(argv[1], "flushreset") == 0) {
            Sleep(2000);
            return Ioctl(IOCTL_S10EDIAG_HARD_RESET, NULL, 0, NULL, 0, NULL);
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "hardreset") == 0) {
        return Ioctl(IOCTL_S10EDIAG_HARD_RESET, NULL, 0, NULL, 0, NULL);
    }
    if (argc >= 3 && strcmp(argv[1], "fallback") == 0) {
        ULONG Seconds = strtoul(argv[2], NULL, 0);
        return Ioctl(IOCTL_S10EDIAG_FALLBACK, &Seconds, sizeof(Seconds), NULL, 0, NULL);
    }

    fprintf(stderr,
            "usage: s10ediag info | peek <phys> [bytes] | dumpram|dumpmmio <phys> <bytes> <file> | poke <phys> <value>\n"
            "                | recovery | hardreset | fallback <seconds>\n");
    return 1;
}
