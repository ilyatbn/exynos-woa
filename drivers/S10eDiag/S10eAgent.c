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
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <winioctl.h>
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

    gLog[0] = CreateFileW(LOCAL_LOG, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LogLine("S10eAgent start");

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
    swprintf_s(Path, ARRAYSIZE(Path), L"%s\\S10EDIAG\\out\\agent.log", gEsp);
    gLog[1] = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LogLine("ESP %ls (attempt %lu)", gEsp, Attempt);

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
