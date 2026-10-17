/*
 * s10emode.exe - list and set the desktop resolution (G20, notes/g20-scaled-modes.md: the 720x1520 scaled mode).
 * The same ChangeDisplaySettingsExW path Settings > Display > Resolution uses; for tests the change is NOT saved by
 * default (no CDS_UPDATEREGISTRY), so a reboot returns to the saved mode - the safety net if a mode turns out black.
 * Runs in the user's session (run-as-user.ps1); no admin needed.
 *
 *   s10emode list                 every mode the driver offers (EnumDisplaySettings) + the current one
 *   s10emode WxH [save]           switch to W x H (the current orientation; W/H as Windows reports them), save = persist
 *   s10emode native [save]        back to the largest mode
 *
 * Exit code 0 = applied; otherwise the DISP_CHANGE_* value. Every call appends one line to %TEMP%\s10emode.log.
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Log(const char *Line)
{
    char path[MAX_PATH];
    FILE *f;
    printf("%s\n", Line);
    if (GetEnvironmentVariableA("TEMP", path, sizeof(path)) == 0)
        return;
    strcat_s(path, sizeof(path), "\\s10emode.log");
    if (fopen_s(&f, path, "a") == 0) {
        fprintf(f, "%s\n", Line);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    DEVMODEW cur = { 0 }, m = { 0 };
    char line[256];
    DWORD w = 0, h = 0;

    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &cur)) {
        Log("EnumDisplaySettings(current) failed");
        return 1;
    }
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        sprintf_s(line, sizeof(line), "current %lux%lu @%lu Hz orientation %lu", cur.dmPelsWidth, cur.dmPelsHeight,
                  cur.dmDisplayFrequency, cur.dmDisplayOrientation);
        Log(line);
        m.dmSize = sizeof(m);
        for (DWORD i = 0; EnumDisplaySettingsW(NULL, i, &m); i++) {
            sprintf_s(line, sizeof(line), "mode %lu: %lux%lu %lu bpp @%lu Hz orientation %lu", i, m.dmPelsWidth,
                      m.dmPelsHeight, m.dmBitsPerPel, m.dmDisplayFrequency, m.dmDisplayOrientation);
            Log(line);
        }
        return 0;
    }
    if (strcmp(argv[1], "native") == 0) {
        m.dmSize = sizeof(m);
        for (DWORD i = 0; EnumDisplaySettingsW(NULL, i, &m); i++)
            if ((ULONGLONG)m.dmPelsWidth * m.dmPelsHeight > (ULONGLONG)w * h) {
                w = m.dmPelsWidth;
                h = m.dmPelsHeight;
            }
        /* No swap for landscape: the listed modes are already in the current orientation (2026-10-17 on the S10e,
         * orientation 1: "mode 0: 2280x1080"). */
    } else if (sscanf_s(argv[1], "%lux%lu", &w, &h) != 2 || w == 0 || h == 0) {
        Log("usage: s10emode list | WxH [save] | native [save]");
        return 1;
    }
    BOOL save = argc > 2 && strcmp(argv[2], "save") == 0;
    m = cur;
    m.dmPelsWidth = w;
    m.dmPelsHeight = h;
    m.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    LONG test = ChangeDisplaySettingsExW(NULL, &m, NULL, CDS_TEST, NULL);
    LONG r = test == DISP_CHANGE_SUCCESSFUL ? ChangeDisplaySettingsExW(NULL, &m, NULL, save ? CDS_UPDATEREGISTRY : 0,
                                                                       NULL)
                                            : test;
    sprintf_s(line, sizeof(line), "set %lux%lu (%s): test %ld -> %ld", w, h, save ? "saved" : "not saved", test, r);
    Log(line);
    return r == DISP_CHANGE_SUCCESSFUL ? 0 : (int)r;
}
