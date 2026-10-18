/*
 * keyplay - replay a keyrec recording (tools/phone/games/keyrec/keyrec.c) with the same timing: each "<ms> D|U <scan>
 * <ext>" line becomes a SendInput scan-code event (DirectInput games read scan codes) at <ms> after keyplay started
 * (start it together with the game - gamedrive.ps1 does). Lines with negative times (keys before the game started)
 * are skipped. NFSU2 race automation, notes/games-nfsu2.md.
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
static void EnsureForeground(const char *proc, int *fixes)
{
    if (!g_pid) g_pid = FindPid(proc);
    if (!g_pid) return;
    DWORD fg = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg);
    if (fg == g_pid) return;
    g_wnd = NULL;
    EnumWindows(FindWnd, 0);
    if (!g_wnd) return;
    INPUT alt[2] = { 0 };
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    SetForegroundWindow(g_wnd);
    (*fixes)++;
}

int main(int argc, char **argv)
{
    const char *file = argc > 1 ? argv[1] : "C:\\S10eTmp\\gamekeys.rec";
    int pct = argc > 2 ? atoi(argv[2]) : 100;
    const char *proc = argc > 3 ? argv[3] : "SPEED2.EXE";
    int fixes = 0;
    FILE *f = fopen(file, "r");
    if (!f) { printf("no %s\n", file); return 2; }
    ULONGLONG t0 = GetTickCount64();
    char line[128];
    int sent = 0;
    while (fgets(line, sizeof(line), f)) {
        long long ms; char du; unsigned long scan; int ext;
        if (line[0] == '#' || sscanf(line, "%lld %c %lx %d", &ms, &du, &scan, &ext) != 4 || ms < 0)
            continue;
        ULONGLONG due = t0 + (ULONGLONG)(ms * 100 / (pct > 0 ? pct : 100));
        ULONGLONG now = GetTickCount64();
        if (due > now) Sleep((DWORD)(due - now));
        EnsureForeground(proc, &fixes);
        INPUT in = { 0 };
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = (WORD)scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (du == 'U' ? KEYEVENTF_KEYUP : 0);
        SendInput(1, &in, sizeof(in));
        sent++;
    }
    fclose(f);
    printf("keyplay: %d events\n", sent);
    return 0;
}
