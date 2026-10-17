/*
 * dbgcap.exe <seconds> <out.txt> - capture OutputDebugString output in this session for <seconds> and append it to
 * <out.txt> as "pid: text" lines. Games bring-up (notes/games-nfsu2.md): dgVoodoo's debug build reports only through
 * the debug output. Uses the documented DBWIN protocol (the one DebugView uses): the shared section DBWIN_BUFFER
 * (4 KiB: DWORD pid + text) and the events DBWIN_BUFFER_READY / DBWIN_DATA_READY, in this session's namespace. Run it
 * in the same session as the program (run-as-user.ps1); only one debug-output listener can own the objects.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    DWORD secs = argc > 1 ? (DWORD)atoi(argv[1]) : 30;
    const char *path = argc > 2 ? argv[2] : "dbgcap.txt";
    HANDLE ready = CreateEventA(NULL, FALSE, FALSE, "DBWIN_BUFFER_READY");
    HANDLE data = CreateEventA(NULL, FALSE, FALSE, "DBWIN_DATA_READY");
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "DBWIN_BUFFER");
    if (!ready || !data || !map) {
        fprintf(stderr, "dbgcap: objects: %lu\n", GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        fprintf(stderr, "dbgcap: another listener owns DBWIN_BUFFER\n");
    struct { DWORD pid; char text[4096 - sizeof(DWORD)]; } *buf = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 4096);
    FILE *f;
    if (!buf || fopen_s(&f, path, "a") != 0)
        return 1;
    fprintf(f, "dbgcap: listening %lu s\n", secs);
    fflush(f);
    ULONGLONG end = GetTickCount64() + (ULONGLONG)secs * 1000;
    unsigned lines = 0;
    SetEvent(ready);
    while (GetTickCount64() < end) {
        if (WaitForSingleObject(data, 200) != WAIT_OBJECT_0)
            continue;
        /* the view is read-only: copy, then terminate the copy (writing the section crashed the first version) */
        static char text[sizeof(buf->text) + 1];
        DWORD pid = buf->pid;
        memcpy(text, buf->text, sizeof(buf->text));
        text[sizeof(buf->text)] = 0;
        size_t n = strlen(text);
        while (n && (text[n - 1] == '\n' || text[n - 1] == '\r'))
            n--;
        fprintf(f, "%lu: %.*s\n", pid, (int)n, text);
        lines++;
        fflush(f);   /* a run-as-user task that ends may kill this process: keep the file current */
        SetEvent(ready);
    }
    fprintf(f, "dbgcap: %u lines\n", lines);
    fclose(f);
    return 0;
}
