/*
 * S10eAgent.exe - LocalSystem service that turns every Windows boot on the S10e into a scripted,
 * self-reporting run. notes/m11-s10ediag.md.
 *
 *  1. mounts the ESP (CACHE, typed as ESP - W30) as S: with `mountvol S: /S`;
 *  2. if S:\S10EDIAG\ exists: logs boot progress to S:\S10EDIAG\out\agent.log every 15 s (uptime,
 *     LogonUI / explorer present) until explorer.exe runs (= desktop) or S10EDIAG\wait.txt seconds pass;
 *  3. runs S:\S10EDIAG\collect.cmd (cmd.exe, OUT=S:\S10EDIAG\out, stdout+stderr -> out\collect.log).
 * What to collect and whether to go back to TWRP is entirely the script's business, so it can be
 * changed from TWRP (vfat) without touching NTFS or rebuilding anything.
 */

#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>

#define AGENT_NAME      L"S10eAgent"
#define ESP_ROOT        L"S:\\"
#define DIAG_DIR        L"S:\\S10EDIAG"
#define OUT_DIR         L"S:\\S10EDIAG\\out"
#define LOG_PATH        L"S:\\S10EDIAG\\out\\agent.log"
#define SCRIPT_PATH     L"S:\\S10EDIAG\\collect.cmd"
#define WAIT_PATH       L"S:\\S10EDIAG\\wait.txt"
#define DEFAULT_WAIT_S  1800UL
#define SCRIPT_LIMIT_MS (60UL * 60UL * 1000UL)

static SERVICE_STATUS_HANDLE gStatusHandle;
static SERVICE_STATUS gStatus;
static HANDLE gStopEvent;
static HANDLE gLog = INVALID_HANDLE_VALUE;

static void
LogLine(const char *Format, ...)
{
    char Line[1024];
    int Used;
    DWORD Written;
    va_list Args;

    if (gLog == INVALID_HANDLE_VALUE) {
        return;
    }
    Used = _snprintf_s(Line, sizeof(Line), _TRUNCATE, "[%10llu ms] ", GetTickCount64());
    va_start(Args, Format);
    Used += _vsnprintf_s(Line + Used, sizeof(Line) - Used, _TRUNCATE, Format, Args);
    va_end(Args);
    if (Used > 0 && Used < (int)sizeof(Line) - 2) {
        Line[Used++] = '\r';
        Line[Used++] = '\n';
    }
    WriteFile(gLog, Line, (DWORD)Used, &Written, NULL);
    FlushFileBuffers(gLog);
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

static DWORD
ReadWaitSeconds(void)
{
    char Text[32] = { 0 };
    DWORD Read = 0;
    HANDLE File = CreateFileW(WAIT_PATH, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

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
    HANDLE Output;
    SECURITY_ATTRIBUTES Inherit = { sizeof(Inherit), NULL, TRUE };

    UNREFERENCED_PARAMETER(Parameter);

    // 1. ESP as S: (mountvol needs the volume to have arrived; retry for ~2 min).
    for (Attempt = 0; Attempt < 24; Attempt++) {
        if (GetFileAttributesW(ESP_ROOT) != INVALID_FILE_ATTRIBUTES) {
            break;
        }
        RunAndWait(L"C:\\Windows\\System32\\mountvol.exe S: /S", 30000, INVALID_HANDLE_VALUE);
        if (GetFileAttributesW(ESP_ROOT) != INVALID_FILE_ATTRIBUTES ||
            WaitForSingleObject(gStopEvent, 5000) == WAIT_OBJECT_0) {
            break;
        }
    }
    if (GetFileAttributesW(DIAG_DIR) == INVALID_FILE_ATTRIBUTES) {
        return 0;   // no S10EDIAG directory on the ESP (or no ESP): nothing to do this boot
    }

    CreateDirectoryW(OUT_DIR, NULL);
    gLog = CreateFileW(LOG_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LogLine("S10eAgent start (ESP mounted after %lu attempts)", Attempt);

    // 2. Wait for the desktop, logging what the logon is doing.
    WaitSeconds = ReadWaitSeconds();
    Start = GetTickCount64();
    for (;;) {
        BOOL LogonUi = ProcessRunning(L"LogonUI.exe");
        BOOL Explorer = ProcessRunning(L"explorer.exe");

        LogLine("waiting: LogonUI=%d explorer=%d", LogonUi, Explorer);
        if (Explorer) {
            LogLine("desktop: explorer.exe is running");
            break;
        }
        if (GetTickCount64() - Start > (ULONGLONG)WaitSeconds * 1000ULL) {
            LogLine("no explorer.exe after %lu s - running the script anyway", WaitSeconds);
            break;
        }
        if (WaitForSingleObject(gStopEvent, 15000) == WAIT_OBJECT_0) {
            LogLine("service stop while waiting");
            return 0;
        }
    }

    // 3. The script.
    if (GetFileAttributesW(SCRIPT_PATH) == INVALID_FILE_ATTRIBUTES) {
        LogLine("no collect.cmd - done");
        return 0;
    }
    SetEnvironmentVariableW(L"OUT", OUT_DIR);
    Output = CreateFileW(L"S:\\S10EDIAG\\out\\collect.log", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         &Inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LogLine("running collect.cmd");
    Exit = RunAndWait(L"C:\\Windows\\System32\\cmd.exe /d /c S:\\S10EDIAG\\collect.cmd", SCRIPT_LIMIT_MS, Output);
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
    if (gLog != INVALID_HANDLE_VALUE) {
        CloseHandle(gLog);
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
