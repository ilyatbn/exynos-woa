/*
 * s10erotate.exe - set the display orientation from inside Windows (user request, notes/m15-perf.md:
 * "switch the screen orientation to landscape ... dynamic with a tool from within windows ... auto rotation
 * later"). Uses the standard ChangeDisplaySettingsExW orientation path, so a future sensor service can call
 * it the same way. Runs in the user's session (the desktop launchers); no admin needed.
 *
 *   s10erotate 0 | 90 | 180 | 270 | cycle | query
 *
 * Exit code 0 = applied; otherwise the DISP_CHANGE_* value (e.g. -2 BADMODE if the display driver does not
 * support rotation). Every call appends one line to %TEMP%\s10erotate.log.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static void
Log(const char *Line)
{
    char Path[MAX_PATH];
    FILE *File;

    if (GetEnvironmentVariableA("TEMP", Path, sizeof(Path)) == 0) {
        return;
    }
    strcat_s(Path, sizeof(Path), "\\s10erotate.log");
    if (fopen_s(&File, Path, "a") == 0) {
        fprintf(File, "%s\n", Line);
        fclose(File);
    }
}

int
main(int argc, char **argv)
{
    DEVMODEW Mode = { 0 };
    DWORD Target;
    LONG Result;
    char Line[256];

    Mode.dmSize = sizeof(Mode);
    if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &Mode)) {
        Log("EnumDisplaySettings failed");
        return 1;
    }

    if (argc < 2 || strcmp(argv[1], "query") == 0) {
        sprintf_s(Line, sizeof(Line), "current %lux%lu orientation %lu (x90 deg)", Mode.dmPelsWidth,
                  Mode.dmPelsHeight, Mode.dmDisplayOrientation);
        printf("%s\n", Line);
        Log(Line);
        return 0;
    }

    if (strcmp(argv[1], "cycle") == 0) {
        Target = (Mode.dmDisplayOrientation + 1) % 4;
    } else {
        Target = (DWORD)(atoi(argv[1]) / 90) % 4;
    }

    // 0/180 <-> 90/270 swaps the width and height.
    if (((Mode.dmDisplayOrientation ^ Target) & 1) != 0) {
        DWORD Swap = Mode.dmPelsWidth;
        Mode.dmPelsWidth = Mode.dmPelsHeight;
        Mode.dmPelsHeight = Swap;
    }
    Mode.dmDisplayOrientation = Target;
    Mode.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;

    Result = ChangeDisplaySettingsExW(NULL, &Mode, NULL, CDS_UPDATEREGISTRY, NULL);
    sprintf_s(Line, sizeof(Line), "rotate to %lu deg -> %ld (%lux%lu)", Target * 90, Result, Mode.dmPelsWidth,
              Mode.dmPelsHeight);
    printf("%s\n", Line);
    Log(Line);
    return Result == DISP_CHANGE_SUCCESSFUL ? 0 : (int)Result;
}
