/*
 * keyrec - record the user's keyboard input while they play, so tools/phone/games/keyplay can replay it (NFSU2 race
 * automation, notes/games-nfsu2.md). Started on purpose in the user's session; records only for SECONDS (or until F12),
 * only keyboard events, into OUTFILE:
 *   "# keyrec v1 ..." header lines, then one event per line:  <ms> D|U <scancode hex> <extended 0|1>
 * Times are relative to the start of PROCESS (default SPEED2.EXE) when it starts during the recording - so a replay
 * started together with the game reproduces the timing - else relative to the recorder start.
 * usage: keyrec [SECONDS=180] [OUTFILE=C:\S10eTmp\gamekeys.rec] [PROCESS=SPEED2.EXE]
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 20000
typedef struct { ULONGLONG t; char down; DWORD scan; int ext; } EV;
static EV g_ev[MAX_EVENTS];
static int g_n;
static volatile LONG g_stop;

static LRESULT CALLBACK Hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_n < MAX_EVENTS) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        if (!(k->flags & LLKHF_INJECTED)) {           /* only real key presses, not our own replays */
            int down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            if (k->vkCode == VK_F12) {
                if (down) InterlockedExchange(&g_stop, 1);
            } else {
                EV *e = &g_ev[g_n++];
                e->t = GetTickCount64();
                e->down = down ? 'D' : 'U';
                e->scan = k->scanCode;
                e->ext = (k->flags & LLKHF_EXTENDED) ? 1 : 0;
            }
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static ULONGLONG ProcessStart(const char *name)
{
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = { sizeof(pe) };
    ULONGLONG start = 0;
    for (BOOL ok = Process32First(s, &pe); ok && !start; ok = Process32Next(s, &pe)) {
        if (_stricmp(pe.szExeFile, name) == 0) {
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            FILETIME c, x, k, u;
            if (p && GetProcessTimes(p, &c, &x, &k, &u)) {
                FILETIME now; GetSystemTimeAsFileTime(&now);
                ULONGLONG ageMs = (((ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime) -
                                   ((ULONGLONG)c.dwHighDateTime << 32 | c.dwLowDateTime)) / 10000;
                start = GetTickCount64() - ageMs;
            }
            if (p) CloseHandle(p);
        }
    }
    CloseHandle(s);
    return start;
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 180;
    const char *out = argc > 2 ? argv[2] : "C:\\S10eTmp\\gamekeys.rec";
    const char *proc = argc > 3 ? argv[3] : "SPEED2.EXE";
    ULONGLONG t0 = GetTickCount64(), launch = 0;
    HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, Hook, GetModuleHandleW(NULL), 0);
    if (!h) return 2;
    SetTimer(NULL, 1, 100, NULL);
    MSG m;
    while (!g_stop && GetTickCount64() - t0 < (ULONGLONG)seconds * 1000 && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!launch) {
            ULONGLONG ps = ProcessStart(proc);
            if (ps && ps >= t0 - 2000) launch = ps;          /* started during (or just before) the recording */
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    UnhookWindowsHookEx(h);
    FILE *f = fopen(out, "w");
    if (!f) return 3;
    ULONGLONG base = launch ? launch : t0;
    fprintf(f, "# keyrec v1: %d events, times relative to %s start%s\n", g_n, launch ? proc : "recorder",
            g_stop ? ", stopped with F12" : "");
    for (int i = 0; i < g_n; i++)
        fprintf(f, "%lld %c %02lX %d\n", (long long)(g_ev[i].t - base), g_ev[i].down, g_ev[i].scan, g_ev[i].ext);
    fclose(f);
    return 0;
}
