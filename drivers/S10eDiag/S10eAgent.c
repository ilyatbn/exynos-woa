/*
 * S10eAgent.exe - LocalSystem service that turns every Windows boot on the S10e into a scripted,
 * self-reporting run. notes/m11-s10ediag.md.
 *
 *  1. finds the volume that holds \S10EDIAG\collect.cmd (the ESP = CACHE, W30) and gives it a drive
 *     letter (keeps an existing one, else the first free of S..Z, D..R; `mountvol S: /S` as the last
 *     resort). Run 1 showed why a fixed S: is not enough: Windows had lettered every Android partition;
 *  2. logs boot progress to <ESP>\S10EDIAG\out\agent.log (and, from the first line, to
 *     C:\Windows\Temp\S10eAgent.log) until explorer.exe has run for 30 s (= desktop) or
 *     S10EDIAG\wait.txt seconds pass;
 *  3. runs <ESP>\S10EDIAG\collect.cmd (cmd.exe, ESP=X:, OUT=X:\S10EDIAG\out, output -> out\collect.log).
 * What to collect and whether to go back to TWRP is entirely the script's business, so it can be
 * changed from TWRP (vfat) without touching NTFS or rebuilding anything.
 *
 * Hardware buttons (user request 2026-10-11, notes/d8-buttons-agent.md): a second thread polls the four key
 * GPIOs through S10eDiag every 25 ms (ALIVE GPIO 0x15850000: gpa0 DAT +0x04 = VOLUP b3 / VOLDN b4 / BIXBY b6,
 * gpa2 DAT +0x44 = POWER b4, all active-low - Exynos9820.h D8 constants), tells short from long presses and runs
 * the action C:\ProgramData\S10e\buttons.ini maps it to, in the console user's session.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <winioctl.h>
#include <wtsapi32.h>
#include <userenv.h>
#include "S10eDiagIoctl.h"

#define AGENT_NAME      L"S10eAgent"
#define MARKER          L"S10EDIAG\\collect.cmd"
#define LOCAL_LOG       L"C:\\Windows\\Temp\\S10eAgent.log"
#define DEFAULT_WAIT_S  1800UL
#define SCRIPT_LIMIT_MS (60UL * 60UL * 1000UL)

static SERVICE_STATUS_HANDLE gStatusHandle;
static SERVICE_STATUS gStatus;
static HANDLE gStopEvent;
static HANDLE gLog[2] = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };
static wchar_t gEsp[4];   // "X:"
static HANDLE gDiag = INVALID_HANDLE_VALUE;   // S10eDiag: every log line also goes to the PRAM mark ring

static void
LogLine(const char *Format, ...)
{
    char Line[1024];
    int Used;
    DWORD Written;
    DWORD Index;
    va_list Args;

    Used = _snprintf_s(Line, sizeof(Line), _TRUNCATE, "[%10llu ms] ", GetTickCount64());
    va_start(Args, Format);
    Used += _vsnprintf_s(Line + Used, sizeof(Line) - Used, _TRUNCATE, Format, Args);
    va_end(Args);
    if (Used > 0 && Used < (int)sizeof(Line) - 2) {
        Line[Used++] = '\r';
        Line[Used++] = '\n';
    }
    for (Index = 0; Index < 2; Index++) {
        if (gLog[Index] != INVALID_HANDLE_VALUE) {
            WriteFile(gLog[Index], Line, (DWORD)Used, &Written, NULL);
            FlushFileBuffers(gLog[Index]);
        }
    }
    if (gDiag == INVALID_HANDLE_VALUE) {
        gDiag = CreateFileW(S10EDIAG_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    }
    if (gDiag != INVALID_HANDLE_VALUE && Used > 18) {
        // Skip the 16-char "[%10llu ms] " prefix (the ring stamps its own uptime) and the CR LF.
        DeviceIoControl(gDiag, IOCTL_S10EDIAG_MARK, Line + 16, (DWORD)(Used - 18), NULL, 0, &Written, NULL);
    }
}

static DWORD
RunAndWait(const wchar_t *CommandLine, DWORD TimeoutMs, HANDLE Output)
{
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Process = { 0 };
    wchar_t Command[1024];
    DWORD Exit = (DWORD)-1;

    wcsncpy_s(Command, ARRAYSIZE(Command), CommandLine, _TRUNCATE);
    if (Output != INVALID_HANDLE_VALUE) {
        Startup.dwFlags = STARTF_USESTDHANDLES;
        Startup.hStdOutput = Output;
        Startup.hStdError = Output;
        Startup.hStdInput = NULL;
    }
    if (!CreateProcessW(NULL, Command, NULL, NULL, Output != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, NULL,
                        NULL, &Startup, &Process)) {
        return (DWORD)-2;
    }
    if (WaitForSingleObject(Process.hProcess, TimeoutMs) == WAIT_TIMEOUT) {
        TerminateProcess(Process.hProcess, 0xDEAD);
    }
    GetExitCodeProcess(Process.hProcess, &Exit);
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
    return Exit;
}

static BOOL
ProcessRunning(const wchar_t *Name)
{
    PROCESSENTRY32W Entry = { sizeof(Entry) };
    HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    BOOL Found = FALSE;

    if (Snapshot == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    if (Process32FirstW(Snapshot, &Entry)) {
        do {
            if (_wcsicmp(Entry.szExeFile, Name) == 0) {
                Found = TRUE;
                break;
            }
        } while (Process32NextW(Snapshot, &Entry));
    }
    CloseHandle(Snapshot);
    return Found;
}

static BOOL
LetterHasMarker(wchar_t Letter)
{
    wchar_t Path[32];

    swprintf_s(Path, ARRAYSIZE(Path), L"%c:\\%s", Letter, MARKER);
    return GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES;
}

//
// Find the volume holding \S10EDIAG\collect.cmd and make sure it has a drive letter. TRUE + gEsp on success.
//
static BOOL
FindEsp(void)
{
    static const wchar_t Order[] = L"STUVWXYZDEFGHIJKLMNOPQR";
    wchar_t Volume[MAX_PATH];
    wchar_t Path[MAX_PATH + 32];
    wchar_t Names[512];
    DWORD Length;
    HANDLE Find;
    BOOL Found = FALSE;
    DWORD Drives;
    DWORD Index;

    Find = FindFirstVolumeW(Volume, ARRAYSIZE(Volume));
    if (Find == INVALID_HANDLE_VALUE) {
        LogLine("FindFirstVolume failed %lu", GetLastError());
        return FALSE;
    }
    do {
        swprintf_s(Path, ARRAYSIZE(Path), L"%s%s", Volume, MARKER);   // Volume ends in '\'
        if (GetFileAttributesW(Path) != INVALID_FILE_ATTRIBUTES) {
            Found = TRUE;
            break;
        }
    } while (FindNextVolumeW(Find, Volume, ARRAYSIZE(Volume)));
    FindVolumeClose(Find);

    if (!Found) {
        LogLine("no volume holds \\%ls (via volume GUID paths)", MARKER);
        return FALSE;
    }
    LogLine("marker volume %ls", Volume);

    // Existing letter?
    if (GetVolumePathNamesForVolumeNameW(Volume, Names, ARRAYSIZE(Names), &Length) && Names[0] != L'\0' &&
        Names[1] == L':') {
        gEsp[0] = Names[0];
        gEsp[1] = L':';
        gEsp[2] = L'\0';
        LogLine("already mounted at %ls", gEsp);
        return TRUE;
    }

    Drives = GetLogicalDrives();
    for (Index = 0; Order[Index] != L'\0'; Index++) {
        wchar_t Root[4] = { Order[Index], L':', L'\\', L'\0' };

        if ((Drives & (1UL << (Order[Index] - L'A'))) != 0) {
            continue;
        }
        if (SetVolumeMountPointW(Root, Volume)) {
            gEsp[0] = Order[Index];
            gEsp[1] = L':';
            gEsp[2] = L'\0';
            LogLine("mounted at %ls", gEsp);
            return TRUE;
        }
        LogLine("SetVolumeMountPoint %ls failed %lu", Root, GetLastError());
    }
    return FALSE;
}

static DWORD
ReadWaitSeconds(void)
{
    char Text[32] = { 0 };
    wchar_t Path[64];
    DWORD Read = 0;
    HANDLE File;

    swprintf_s(Path, ARRAYSIZE(Path), L"%s\\S10EDIAG\\wait.txt", gEsp);
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE) {
        return DEFAULT_WAIT_S;
    }
    ReadFile(File, Text, sizeof(Text) - 1, &Read, NULL);
    CloseHandle(File);
    return (Read > 0) ? strtoul(Text, NULL, 10) : DEFAULT_WAIT_S;
}

static DWORD WINAPI
Worker(LPVOID Parameter)
{
    DWORD Attempt;
    DWORD WaitSeconds;
    DWORD Exit;
    ULONGLONG Start;
    ULONGLONG ExplorerSince = 0;
    ULONGLONG LastLog = 0;
    HANDLE Output;
    wchar_t Path[MAX_PATH];
    wchar_t Command[MAX_PATH];
    SECURITY_ATTRIBUTES Inherit = { sizeof(Inherit), NULL, TRUE };

    UNREFERENCED_PARAMETER(Parameter);

    // Keep the previous boot's log: buttons test 1 lost the evidence when the user rebooted from the power menu.
    MoveFileExW(LOCAL_LOG, LOCAL_LOG L".prev", MOVEFILE_REPLACE_EXISTING);
    gLog[0] = CreateFileW(LOCAL_LOG, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LogLine("S10eAgent start");

    // 0. Requests are only valid for the session they were made in. Fresh install 2026-10-11: the user
    // clicked "Reboot to TWRP" on the first boot, before this service existed; the file survived and every
    // later boot went to TWRP right after the desktop came up (the delete before flushreset was lost once).
    if (DeleteFileW(L"C:\\S10eTmp\\req\\recovery.req")) {
        LogLine("stale request discarded: recovery.req");
    }
    if (DeleteFileW(L"C:\\S10eTmp\\req\\collect.req")) {
        LogLine("stale request discarded: collect.req");
    }

    // 1. The ESP (volumes can arrive late on one core: retry for ~5 min).
    for (Attempt = 0; Attempt < 30; Attempt++) {
        if (FindEsp()) {
            break;
        }
        if (Attempt == 10) {
            LogLine("trying mountvol S: /S");
            RunAndWait(L"C:\\Windows\\System32\\mountvol.exe S: /S", 30000, INVALID_HANDLE_VALUE);
            if (LetterHasMarker(L'S')) {
                wcscpy_s(gEsp, ARRAYSIZE(gEsp), L"S:");
                break;
            }
        }
        if (WaitForSingleObject(gStopEvent, 10000) == WAIT_OBJECT_0) {
            return 0;
        }
    }
    if (gEsp[0] == L'\0') {
        LogLine("no ESP with \\S10EDIAG - nothing to do this boot");
        return 0;
    }

    swprintf_s(Path, ARRAYSIZE(Path), L"%s\\S10EDIAG\\out", gEsp);
    CreateDirectoryW(Path, NULL);
    SetEnvironmentVariableW(L"OUT", Path);
    SetEnvironmentVariableW(L"ESP", gEsp);
    {
        wchar_t Prev[MAX_PATH];

        swprintf_s(Path, ARRAYSIZE(Path), L"%s\\S10EDIAG\\out\\agent.log", gEsp);
        swprintf_s(Prev, ARRAYSIZE(Prev), L"%s.prev", Path);
        MoveFileExW(Path, Prev, MOVEFILE_REPLACE_EXISTING);
    }
    gLog[1] = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LogLine("ESP %ls (attempt %lu)", gEsp, Attempt);

    // Unattended runs: a hang after this point (no bugcheck, no restart) must still end in TWRP. S10eDiag's
    // timer hard-resets to recovery after 20 min; collect.cmd's restart ends the boot long before that.
    swprintf_s(Path, ARRAYSIZE(Path), L"%s\\S10EDIAG\\stay.flg", gEsp);
    if (GetFileAttributesW(Path) == INVALID_FILE_ATTRIBUTES && gDiag != INVALID_HANDLE_VALUE) {
        ULONG Seconds = 1200;
        DWORD Returned;

        if (DeviceIoControl(gDiag, IOCTL_S10EDIAG_FALLBACK, &Seconds, sizeof(Seconds), NULL, 0, &Returned, NULL)) {
            LogLine("fallback armed: hard reset to TWRP in %lu s", Seconds);
        }
    }

    // 2. Wait for the desktop, logging what the logon is doing.
    // User: explorer.exe up for 30 s is the "desktop is up" signal; poll every 5 s, log every 15 s.
    WaitSeconds = ReadWaitSeconds();
    Start = GetTickCount64();
    for (;;) {
        BOOL LogonUi = ProcessRunning(L"LogonUI.exe");
        BOOL Explorer = ProcessRunning(L"explorer.exe");
        ULONGLONG Now = GetTickCount64();

        if (!Explorer) {
            ExplorerSince = 0;
        } else if (ExplorerSince == 0) {
            ExplorerSince = Now;
            LogLine("explorer.exe appeared");
        }
        if (Now - LastLog >= 15000) {
            LogLine("waiting: LogonUI=%d explorer=%d", LogonUi, Explorer);
            LastLog = Now;
        }
        if (Explorer && Now - ExplorerSince >= 30000) {
            LogLine("desktop: explorer.exe up for 30 s");
            break;
        }
        if (GetTickCount64() - Start > (ULONGLONG)WaitSeconds * 1000ULL) {
            LogLine("no explorer.exe after %lu s - running the script anyway", WaitSeconds);
            break;
        }
        if (WaitForSingleObject(gStopEvent, 5000) == WAIT_OBJECT_0) {
            LogLine("service stop while waiting");
            return 0;
        }
    }

    // 3. The script.
    swprintf_s(Path, ARRAYSIZE(Path), L"%s\\S10EDIAG\\out\\collect.log", gEsp);
    Output = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &Inherit, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    swprintf_s(Command, ARRAYSIZE(Command), L"C:\\Windows\\System32\\cmd.exe /d /c %s\\S10EDIAG\\collect.cmd", gEsp);
    LogLine("running %ls", Command);
    Exit = RunAndWait(Command, SCRIPT_LIMIT_MS, Output);
    if (Output != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(Output);
        CloseHandle(Output);
    }
    LogLine("collect.cmd exit %lu", Exit);

    //
    // 4. User requests (user: "a reboot script (to recovery would be great) or any other stuff that might be
    // helpful when you need me to interact with the ui"). The desktop shortcuts collect.cmd creates only drop a
    // file into C:\S10eTmp\req (writable by Users); this LocalSystem service acts on it, so no UAC prompt:
    //   recovery.req -> s10ediag recovery + clean restart (the PSCI restart bugcheck also lands in TWRP)
    //   collect.req  -> run collect.cmd again with STAY=1 (logs refreshed, Windows keeps running)
    //
    for (;;) {
        if (GetFileAttributesW(L"C:\\S10eTmp\\req\\recovery.req") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(L"C:\\S10eTmp\\req\\recovery.req");
            LogLine("request: reboot to TWRP");
            // m14 runs 18-20: a clean restart loops back into Windows; flush + hard reset is the proven way.
            RunAndWait(L"C:\\Windows\\System32\\s10ediag.exe flushreset", 60000, INVALID_HANDLE_VALUE);
        }
        if (GetFileAttributesW(L"C:\\S10eTmp\\req\\collect.req") != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(L"C:\\S10eTmp\\req\\collect.req");
            LogLine("request: collect logs");
            SetEnvironmentVariableW(L"STAY", L"1");
            swprintf_s(Path, ARRAYSIZE(Path), L"%s\\S10EDIAG\\out\\collect.log", gEsp);
            Output = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &Inherit, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
            Exit = RunAndWait(Command, SCRIPT_LIMIT_MS, Output);
            if (Output != INVALID_HANDLE_VALUE) {
                FlushFileBuffers(Output);
                CloseHandle(Output);
            }
            SetEnvironmentVariableW(L"STAY", NULL);
            LogLine("request: collect done, exit %lu", Exit);
        }
        if (WaitForSingleObject(gStopEvent, 2000) == WAIT_OBJECT_0) {
            return 0;
        }
    }
}


//
// Hardware buttons. Polling (not GpioClx interrupts): no new kernel driver, no conflict with the touch controller's
// ALIVE EINT (GSIV 45, notes/drivers-branch-port-plan.md), and every press is visible here so all actions are
// configurable. 25 ms period, 2-sample debounce, long press fires as soon as the threshold is reached.
//
#define KEYS_ALIVE_GPA0_DAT   0x15850004ULL   // EXYNOS_ALIVE_GPIO_BASE + GPA0_OFF + DAT(+4), VERIFIED (D8 constants)
#define KEYS_ALIVE_GPA2_DAT   0x15850044ULL   // + GPA2_OFF 0x040 + DAT
#define KEYS_CONFIG           L"C:\\ProgramData\\S10e\\buttons.ini"
#define KEYS_PERIOD_MS        25

typedef struct {
    const wchar_t *Name;        // ini key prefix
    int Bank;                   // 0 = gpa0, 2 = gpa2
    unsigned long Bit;
    const wchar_t *DefShort;
    const wchar_t *DefLong;
    int Down;                   // debounced state
    int Raw;                    // last raw sample
    ULONGLONG Since;            // debounced press time
    int LongFired;
} KEY;

static KEY gKeys[] = {
    { L"power",   2, 1UL << 4, L"screen",  L"powermenu", 0, 0, 0, 0 },
    { L"volup",   0, 1UL << 3, L"none",    L"none",      0, 0, 0, 0 },
    { L"voldown", 0, 1UL << 4, L"none",    L"none",      0, 0, 0, 0 },
    { L"bixby",   0, 1UL << 6, L"taskmgr", L"rotate",    0, 0, 0, 0 },
};

static BOOL
KeysRead32(HANDLE Diag, unsigned long long Address, unsigned long *Value)
{
    S10EDIAG_READ_IN In = { Address, 4, S10EDIAG_READ_MMIO };
    DWORD Returned = 0;

    return DeviceIoControl(Diag, IOCTL_S10EDIAG_READ_PHYS, &In, sizeof(In), Value, sizeof(*Value), &Returned, NULL) &&
           (Returned == sizeof(*Value));
}

static void
KeysWriteDefaultConfig(void)
{
    HANDLE File;
    static const char Text[] =
        "; S10e hardware buttons - S10eAgent re-reads this file on every press (no restart needed).\r\n"
        "; Actions: none | screen | powermenu | wake | taskmgr | rotate | reboot | recovery | shutdown | run:<command line>\r\n"
        ";   screen = toggle display, powermenu = Reboot/Recovery/Shut down/Cancel popup, recovery = reboot to TWRP.\r\n"
        "; run: commands start in your session, e.g. run:C:\\Windows\\System32\\osk.exe\r\n"
        "[buttons]\r\n"
        "version=2\r\n"
        "long_press_ms=700\r\n"
        "power.short=screen\r\npower.long=powermenu\r\n"
        "volup.short=none\r\nvolup.long=none\r\n"
        "voldown.short=none\r\nvoldown.long=none\r\n"
        "bixby.short=taskmgr\r\nbixby.long=rotate\r\n";
    DWORD Written;

    CreateDirectoryW(L"C:\\ProgramData\\S10e", NULL);
    File = CreateFileW(KEYS_CONFIG, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File != INVALID_HANDLE_VALUE) {
        WriteFile(File, Text, (DWORD)(sizeof(Text) - 1), &Written, NULL);
        CloseHandle(File);
        LogLine("keys: wrote default %ls", KEYS_CONFIG);
    } else if (GetPrivateProfileIntW(L"buttons", L"version", 1, KEYS_CONFIG) < 2) {
        wchar_t Value[64];

        // v1 -> v2 (user, 2026-10-12): Bixby long press rotates. Only an untouched "none" is changed.
        GetPrivateProfileStringW(L"buttons", L"bixby.long", L"none", Value, ARRAYSIZE(Value), KEYS_CONFIG);
        if (_wcsicmp(Value, L"none") == 0) {
            WritePrivateProfileStringW(L"buttons", L"bixby.long", L"rotate", KEYS_CONFIG);
        }
        WritePrivateProfileStringW(L"buttons", L"version", L"2", KEYS_CONFIG);
        LogLine("keys: config migrated to v2 (bixby.long was %ls)", Value);
    }
}

// Start CommandLine in the active console session as its user (the agent is LocalSystem in session 0). Elevated:
// use the user's linked full token (UAC split token; LocalSystem holds SeTcbPrivilege, so it gets a usable primary
// token) - Taskmgr is highestAvailable and refuses the filtered token (buttons test 1: Bixby did nothing).
static void
KeysRunInSession(const wchar_t *CommandLine, BOOL Elevated)
{
    HANDLE Token = NULL;
    void *Environment = NULL;
    STARTUPINFOW Startup = { sizeof(Startup) };
    PROCESS_INFORMATION Process;
    wchar_t Buffer[512];
    DWORD Session = WTSGetActiveConsoleSessionId();

    if ((Session == 0xFFFFFFFF) || !WTSQueryUserToken(Session, &Token)) {
        LogLine("keys: no console user (session %lu, error %lu) for '%ls'", Session, GetLastError(), CommandLine);
        return;
    }
    if (Elevated) {
        TOKEN_LINKED_TOKEN Linked = { NULL };
        DWORD Size = 0;

        if (GetTokenInformation(Token, TokenLinkedToken, &Linked, sizeof(Linked), &Size) && (Linked.LinkedToken != NULL)) {
            CloseHandle(Token);
            Token = Linked.LinkedToken;
        } else {
            LogLine("keys: no linked token (%lu), running '%ls' unelevated", GetLastError(), CommandLine);
        }
    }
    CreateEnvironmentBlock(&Environment, Token, FALSE);
    wcscpy_s(Buffer, ARRAYSIZE(Buffer), CommandLine);
    Startup.lpDesktop = L"winsta0\\default";
    if (CreateProcessAsUserW(Token, NULL, Buffer, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT, Environment, NULL,
                             &Startup, &Process)) {
        CloseHandle(Process.hThread);
        CloseHandle(Process.hProcess);
    } else {
        LogLine("keys: CreateProcessAsUser '%ls' failed %lu", CommandLine, GetLastError());
    }
    if (Environment != NULL) {
        DestroyEnvironmentBlock(Environment);
    }
    CloseHandle(Token);
}

static void
KeysAction(const KEY *Key, BOOL Long)
{
    wchar_t Name[64];
    wchar_t Action[400];

    swprintf_s(Name, ARRAYSIZE(Name), L"%s.%s", Key->Name, Long ? L"long" : L"short");
    GetPrivateProfileStringW(L"buttons", Name, Long ? Key->DefLong : Key->DefShort, Action, ARRAYSIZE(Action),
                             KEYS_CONFIG);
    LogLine("keys: %ls -> %ls", Name, Action);
    if ((_wcsicmp(Action, L"screen") == 0) || (_wcsicmp(Action, L"powermenu") == 0) || (_wcsicmp(Action, L"wake") == 0)) {
        wchar_t Command[96];

        swprintf_s(Command, ARRAYSIZE(Command), L"C:\\Windows\\System32\\s10ekeys.exe %s", Action);
        KeysRunInSession(Command, FALSE);
    } else if (_wcsicmp(Action, L"rotate") == 0) {
        KeysRunInSession(L"C:\\Windows\\System32\\s10erotate.exe toggle", FALSE);
    } else if (_wcsicmp(Action, L"taskmgr") == 0) {
        KeysRunInSession(L"C:\\Windows\\System32\\Taskmgr.exe", TRUE);
    } else if (_wcsicmp(Action, L"reboot") == 0) {
        RunAndWait(L"C:\\Windows\\System32\\shutdown.exe /r /t 0", 10000, INVALID_HANDLE_VALUE);
    } else if (_wcsicmp(Action, L"shutdown") == 0) {
        RunAndWait(L"C:\\Windows\\System32\\shutdown.exe /s /t 0", 10000, INVALID_HANDLE_VALUE);
    } else if (_wcsicmp(Action, L"recovery") == 0) {
        RunAndWait(L"C:\\Windows\\System32\\s10ediag.exe flushreset", 60000, INVALID_HANDLE_VALUE);
    } else if (_wcsnicmp(Action, L"run:", 4) == 0) {
        KeysRunInSession(Action + 4, FALSE);
    }
}

static DWORD WINAPI
KeysThread(LPVOID Parameter)
{
    HANDLE Diag;
    unsigned long Dat[3] = { 0, 0, 0 };
    UINT LongMs;
    size_t Index;

    UNREFERENCED_PARAMETER(Parameter);
    KeysWriteDefaultConfig();
    Diag = CreateFileW(S10EDIAG_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (Diag == INVALID_HANDLE_VALUE) {
        LogLine("keys: S10eDiag not available (%lu) - buttons disabled", GetLastError());
        return 1;
    }
    if (!KeysRead32(Diag, KEYS_ALIVE_GPA0_DAT, &Dat[0]) || !KeysRead32(Diag, KEYS_ALIVE_GPA2_DAT, &Dat[2])) {
        LogLine("keys: GPIO read failed (%lu) - buttons disabled", GetLastError());
        CloseHandle(Diag);
        return 1;
    }
    LogLine("keys: polling, gpa0 DAT %08lx gpa2 DAT %08lx (keys are active-low)", Dat[0], Dat[2]);
    for (Index = 0; Index < ARRAYSIZE(gKeys); Index++) {
        gKeys[Index].Raw = ((Dat[gKeys[Index].Bank] & gKeys[Index].Bit) == 0);
        gKeys[Index].Down = gKeys[Index].Raw;     // a key held at service start is not a press
        gKeys[Index].LongFired = gKeys[Index].Down;
    }

    while (WaitForSingleObject(gStopEvent, KEYS_PERIOD_MS) == WAIT_TIMEOUT) {
        ULONGLONG Now = GetTickCount64();

        if (!KeysRead32(Diag, KEYS_ALIVE_GPA0_DAT, &Dat[0]) || !KeysRead32(Diag, KEYS_ALIVE_GPA2_DAT, &Dat[2])) {
            continue;
        }
        LongMs = GetPrivateProfileIntW(L"buttons", L"long_press_ms", 700, KEYS_CONFIG);
        for (Index = 0; Index < ARRAYSIZE(gKeys); Index++) {
            KEY *Key = &gKeys[Index];
            int Raw = ((Dat[Key->Bank] & Key->Bit) == 0);

            if ((Raw == Key->Raw) && (Raw != Key->Down)) {   // two equal samples in a row = debounced edge
                Key->Down = Raw;
                if (Raw) {
                    LogLine("keys: %ls down", Key->Name);
                } else {
                    LogLine("keys: %ls up after %llu ms", Key->Name, Now - Key->Since);
                }
                if (Raw) {
                    Key->Since = Now;
                    Key->LongFired = 0;
                } else if (!Key->LongFired) {
                    KeysAction(Key, FALSE);
                }
            }
            Key->Raw = Raw;
            if (Key->Down && !Key->LongFired && ((Now - Key->Since) >= LongMs)) {
                Key->LongFired = 1;
                KeysAction(Key, TRUE);
            }
        }
    }
    CloseHandle(Diag);
    return 0;
}

static DWORD WINAPI
Control(DWORD Code, DWORD EventType, LPVOID EventData, LPVOID Context)
{
    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(Context);

    if (Code == SERVICE_CONTROL_STOP || Code == SERVICE_CONTROL_SHUTDOWN) {
        LogLine("service control %lu", Code);
        gStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(gStatusHandle, &gStatus);
        SetEvent(gStopEvent);
    }
    return NO_ERROR;
}

static void WINAPI
ServiceMain(DWORD Argc, LPWSTR *Argv)
{
    HANDLE Thread;
    DWORD Index;

    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    gStatusHandle = RegisterServiceCtrlHandlerExW(AGENT_NAME, Control, NULL);
    if (gStatusHandle == NULL) {
        return;
    }
    gStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    gStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gStatus.dwCurrentState = SERVICE_RUNNING;
    gStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(gStatusHandle, &gStatus);

    CloseHandle(CreateThread(NULL, 0, KeysThread, NULL, 0, NULL));   // buttons run independently of the ESP work
    Thread = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    if (Thread != NULL) {
        HANDLE Waits[2] = { Thread, gStopEvent };
        WaitForMultipleObjects(2, Waits, FALSE, INFINITE);
        CloseHandle(Thread);
    }
    // Stay running until stopped so `sc query` shows the agent was alive this boot.
    WaitForSingleObject(gStopEvent, INFINITE);
    for (Index = 0; Index < 2; Index++) {
        if (gLog[Index] != INVALID_HANDLE_VALUE) {
            CloseHandle(gLog[Index]);
        }
    }
    gStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(gStatusHandle, &gStatus);
}

int
wmain(void)
{
    SERVICE_TABLE_ENTRYW Table[] = { { (LPWSTR)AGENT_NAME, ServiceMain }, { NULL, NULL } };

    return StartServiceCtrlDispatcherW(Table) ? 0 : 1;
}
