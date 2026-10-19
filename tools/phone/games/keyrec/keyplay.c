/*
 * keyplay - replay a keyrec recording (tools/phone/games/keyrec/keyrec.c) with the same timing: each "<ms> D|U <scan>
 * <ext>" line becomes a SendInput scan-code event (DirectInput games read scan codes) at <ms> after keyplay started
 * (start it together with the game - gamedrive.ps1 does). Lines with negative times (keys before the game started)
 * are skipped. NFSU2 race automation, notes/games-nfsu2.md. v2 (notes/games-unity.md): "<ms> M LD|LU|RD|RU <x> <y>" lines
 * become absolute mouse moves + button events at those physical pixels, tagged KEYPLAY_EXTRA for keyrec.
 * usage: keyplay [FILE=C:\S10eTmp\gamekeys.rec] [SPEED_PERCENT=100] [PROCESS=SPEED2.EXE]
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game must own the foreground: started from a task it often does not, and NFSU2 dies (+0x6562b) when its
 * fullscreen swapchain is reset without focus - the first replayed Enter crashed it (2026-10-18). Before every event:
 * if the game's window is not the foreground window, earn foreground rights the documented way (this process sends
 * the last input event: an Alt tap) and SetForegroundWindow() the game's main window. */
#define KEYPLAY_EXTRA 0x4B455950u
static ULONGLONG g_last_down;   /* when the last replayed mouse button went down */
static DWORD g_pid;
static HWND g_wnd;
static BOOL CALLBACK FindWnd(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL) { g_wnd = h; return FALSE; }
    (void)lp;
    return TRUE;
}
static DWORD FindPid(const char *name)
{
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = { sizeof(pe) };
    DWORD pid = 0;
    for (BOOL ok = Process32First(s, &pe); ok && !pid; ok = Process32Next(s, &pe))
        if (_stricmp(pe.szExeFile, name) == 0) pid = pe.th32ProcessID;
    CloseHandle(s);
    return pid;
}
static int IsGameForeground(void)
{
    DWORD fg = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg);
    return g_pid && fg == g_pid;
}
/* 2026-10-18 (notes/perf-threaded-context.md): one Alt + SetForegroundWindow try was not always enough - 2 of 3
 * runs still died at the first Enter (+0x6562b). Retry for up to 3 s until the game really owns the foreground
 * before the event is sent; failures go to C:\S10eTmp\keyplay.log. */
static void EnsureForeground(const char *proc, int *fixes, int *fails, FILE *log, long long ms)
{
    if (!g_pid) g_pid = FindPid(proc);
    if (!g_pid || IsGameForeground()) return;
    for (int tries = 0; tries < 30; tries++) {
        g_wnd = NULL;
        EnumWindows(FindWnd, 0);
        if (g_wnd) {
            INPUT alt[2] = { 0 };
            alt[0].type = alt[1].type = INPUT_KEYBOARD;
            alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
            alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, alt, sizeof(INPUT));
            SetForegroundWindow(g_wnd);
        }
        Sleep(100);
        if (IsGameForeground()) {
            (*fixes)++;
            if (log) fprintf(log, "%lld ms: foreground fixed after %d tries\n", ms, tries + 1);
            return;
        }
    }
    (*fails)++;
    if (log) fprintf(log, "%lld ms: game NOT foreground after 3 s (window %p)\n", ms, (void *)g_wnd);
}

int main(int argc, char **argv)
{
    const char *file = argc > 1 ? argv[1] : "C:\\S10eTmp\\gamekeys.rec";
    int pct = argc > 2 ? atoi(argv[2]) : 100;
    const char *proc = argc > 3 ? argv[3] : "SPEED2.EXE";
    int fixes = 0, fails = 0;
    FILE *f = fopen(file, "r");
    if (!f) { printf("no %s\n", file); return 2; }
    FILE *log = fopen("C:\\S10eTmp\\keyplay.log", "w");
    SetProcessDPIAware();   /* v2: recorded mouse positions are physical pixels */
    ULONGLONG t0 = GetTickCount64();
    char line[128];
    int sent = 0;
    while (fgets(line, sizeof(line), f)) {
        long long ms; char du = 0; unsigned long scan = 0; int ext = 0;
        char btn[4] = ""; long mx = 0, my = 0;
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%lld M %3s %ld %ld", &ms, btn, &mx, &my) == 4) {
            if (ms < 0)
                continue;
            du = 'M';
        } else if (sscanf(line, "%lld %c %lx %d", &ms, &du, &scan, &ext) != 4 || ms < 0) {
            continue;
        }
        ULONGLONG due = t0 + (ULONGLONG)(ms * 100 / (pct > 0 ? pct : 100));
        ULONGLONG now = GetTickCount64();
        if (due > now) Sleep((DWORD)(due - now));
        EnsureForeground(proc, &fixes, &fails, log, ms);
        if (du == 'M') {
            int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
            INPUT mi[2] = { 0 };
            mi[0].type = mi[1].type = INPUT_MOUSE;
            mi[0].mi.dx = w > 1 ? (LONG)(mx * 65535 / (w - 1)) : 0;
            mi[0].mi.dy = h > 1 ? (LONG)(my * 65535 / (h - 1)) : 0;
            mi[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            mi[1].mi.dx = mi[0].mi.dx;
            mi[1].mi.dy = mi[0].mi.dy;
            mi[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE |
                               (btn[0] == 'L' ? (btn[1] == 'D' ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP)
                                              : (btn[1] == 'D' ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP));
            mi[0].mi.dwExtraInfo = mi[1].mi.dwExtraInfo = KEYPLAY_EXTRA;
            /* Touch taps are recorded with down and up in the same millisecond; replayed like that Unity (~20 fps)
             * saw press and release inside one frame and ignored the click (davebench run 1, user had to tap). Move
             * first, let a frame pass, and hold a press for at least 150 ms. */
            SendInput(1, &mi[0], sizeof(INPUT));
            Sleep(60);
            if (btn[1] == 'U') {
                ULONGLONG held = GetTickCount64() - g_last_down;
                if (held < 150)
                    Sleep((DWORD)(150 - held));
            }
            SendInput(1, &mi[1], sizeof(INPUT));
            if (btn[1] == 'D')
                g_last_down = GetTickCount64();
            sent++;
            continue;
        }
        INPUT in = { 0 };
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = (WORD)scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (du == 'U' ? KEYEVENTF_KEYUP : 0);
        SendInput(1, &in, sizeof(in));
        sent++;
    }
    fclose(f);
    if (log) { fprintf(log, "keyplay: %d events, foreground fixed %d, failed %d\n", sent, fixes, fails); fclose(log); }
    printf("keyplay: %d events\n", sent);
    return 0;
}
