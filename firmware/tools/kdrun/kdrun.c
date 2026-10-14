/*
 * kdrun.exe - unattended kernel-debugger session for the S10e (host x64, dbgeng from the Debugging Tools).
 * notes/m14-smp.md "M4 under KD": the M4 big cores hang/kill Windows early; a person is not always there to press
 * Ctrl+Break, so this does it:
 *
 *   kdrun <log> <total_s> <break_after_s> [reboot|twrp|go] [<initial-break commands file>]
 *
 *   - attaches to KDNET (net:port=50005,key=1.2.3.4 - the phone's DBG2/BCD settings, notes/m17-usb-kd.md)
 *   - every event that breaks in (bugcheck, exception, initial break) runs the diagnostics below and continues,
 *     or reboots the target when "reboot" is given and the break was a bugcheck
 *   - break_after_s seconds after the debugger first connected it breaks in by itself (a hang leaves no event),
 *     runs the diagnostics, then continues (or reboots with "reboot")
 *   - stops after total_s seconds; all debugger output goes to <log> (and stdout)
 *
 *   twrp: after the diagnostics of the first bugcheck or of the timed break-in, reset the phone into TWRP the way
 *   S10eDiag's hard reset does (progress 0x81, PMU INFORM3 = recovery, RSTSTAT = 0, SWRESET; constants VERIFIED in
 *   drivers/S10eDiag/S10eDiag.c) and stop - the unattended way out of a hung test image.
 *
 *   commands file: the engine asks the kernel for an initial break when it connects (KDNET connects in phase-0 kernel
 *   init, before the secondary processors are started); at that first break every non-empty line of the file is
 *   executed (e.g. ".reload", "bp nt!KeStartAllProcessors") instead of the diagnostics, then the target continues.
 *   "go" = neither reboot nor twrp, so a commands file can follow.
 *
 * Diagnostics: !analyze -v (on a bugcheck), !running -it (every processor's current thread + stack), !prcb per CPU,
 * k on the current processor, lm, vertarget.
 */

#include <windows.h>
#include <stdio.h>
#include <share.h>
#include <initguid.h>
#include <dbgeng.h>

#define DBGENG_PATH "C:\\Program Files (x86)\\Windows Kits\\10\\Debuggers\\x64\\dbgeng.dll"

typedef HRESULT (STDAPICALLTYPE *KDRUN_DEBUG_CREATE)(REFIID InterfaceId, PVOID *Interface);

static FILE *gLog;
static volatile LONG gSawConnect;   /* the engine printed "Connected to" / "Kernel Debugger connection established" */

/* ---- output callbacks -> log + stdout ---- */
static HRESULT STDMETHODCALLTYPE OutQueryInterface(IDebugOutputCallbacks *This, REFIID Iid, void **Out)
{
    if (IsEqualIID(Iid, &IID_IUnknown) || IsEqualIID(Iid, &IID_IDebugOutputCallbacks)) {
        *Out = This;
        return S_OK;
    }
    *Out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE OutAddRef(IDebugOutputCallbacks *This) { (void)This; return 1; }
static ULONG STDMETHODCALLTYPE OutRelease(IDebugOutputCallbacks *This) { (void)This; return 0; }
static HRESULT STDMETHODCALLTYPE OutOutput(IDebugOutputCallbacks *This, ULONG Mask, PCSTR Text)
{
    (void)This;
    (void)Mask;
    if (strstr(Text, "Connected to") || strstr(Text, "connection established"))
        InterlockedExchange(&gSawConnect, 1);
    fputs(Text, gLog);
    fflush(gLog);
    fputs(Text, stdout);
    return S_OK;
}
static IDebugOutputCallbacksVtbl gOutVtbl = { OutQueryInterface, OutAddRef, OutRelease, OutOutput };
static IDebugOutputCallbacks gOut = { &gOutVtbl };

static void Note(const char *Text)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(gLog, "\n==== kdrun %02u:%02u:%02u %s ====\n", t.wHour, t.wMinute, t.wSecond, Text);
    fflush(gLog);
    printf("\n==== kdrun %02u:%02u:%02u %s ====\n", t.wHour, t.wMinute, t.wSecond, Text);
}

static void Run(IDebugControl *Control, const char *Command)
{
    char Line[256];
    _snprintf_s(Line, sizeof(Line), _TRUNCATE, "> %s", Command);
    Note(Line);
    Control->lpVtbl->Execute(Control, DEBUG_OUTCTL_ALL_CLIENTS, Command, DEBUG_EXECUTE_ECHO);
}

static void ResetToTwrp(IDebugControl *Control)
{
    Note("resetting the phone into TWRP (S10eDiag hard-reset sequence)");
    Run(Control, "!ed [uc] 8fe0f004 81");
    Run(Control, "!ed [uc] 1586080c 12345674");
    Run(Control, "!ed [uc] 15860404 0");
    Run(Control, "!ed [uc] 15863a00 2");
}

static void RunFile(IDebugControl *Control, const char *Path)
{
    FILE *F;
    char Line[256];

    if (fopen_s(&F, Path, "r") != 0 || F == NULL) {
        Note("cannot open the initial-break commands file");
        return;
    }
    while (fgets(Line, sizeof(Line), F) != NULL) {
        Line[strcspn(Line, "\r\n")] = 0;
        if (Line[0])
            Run(Control, Line);
    }
    fclose(F);
}

static void Diagnose(IDebugControl *Control, BOOL Bugcheck)
{
    ULONG Count = 0, I;
    char Cmd[64];

    Run(Control, "vertarget");
    if (Bugcheck)
        Run(Control, "!analyze -v");
    Run(Control, "!running -it");
    Control->lpVtbl->GetNumberProcessors(Control, &Count);
    for (I = 0; I < Count && I < 8; I++) {
        _snprintf_s(Cmd, sizeof(Cmd), _TRUNCATE, "!prcb %lu", I);
        Run(Control, Cmd);
    }
    Run(Control, "k 40");
    Run(Control, "lm");
}

int main(int argc, char **argv)
{
    IDebugClient *Client = NULL;
    IDebugControl *Control = NULL;
    ULONGLONG Start, Connected = 0;
    ULONG TotalS, BreakS;
    BOOL Reboot, Twrp, BrokeIn = FALSE, FirstBreak = TRUE;
    const char *InitCmds;
    HRESULT Hr;
    HMODULE Engine;
    KDRUN_DEBUG_CREATE Create;

    if (argc < 4) {
        fprintf(stderr, "usage: kdrun <log> <total_s> <break_after_s> [reboot|twrp|go] [init-cmds]\n");
        return 1;
    }
    /* shared, so the log can be read while a session runs (fopen_s opens it exclusively) */
    gLog = _fsopen(argv[1], "a", _SH_DENYNO);
    if (gLog == NULL)
        return 2;
    TotalS = strtoul(argv[2], NULL, 10);
    BreakS = strtoul(argv[3], NULL, 10);
    Reboot = argc > 4 && _stricmp(argv[4], "reboot") == 0;
    Twrp = argc > 4 && _stricmp(argv[4], "twrp") == 0;
    InitCmds = argc > 5 ? argv[5] : NULL;

    /* The Kits dbgeng.dll by full path: a plain import resolves to the inbox System32\dbgeng.dll (10.0.26100.1),
     * which never connected to the phone (notes/m14-smp.md, kdtest 21:42). ALTERED_SEARCH_PATH makes its own
     * dependencies (dbghelp, dbgmodel, kdnet transports) load from the Kits folder too. */
    Engine = LoadLibraryExA(DBGENG_PATH, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    Create = Engine ? (KDRUN_DEBUG_CREATE)GetProcAddress(Engine, "DebugCreate") : NULL;
    if (Create == NULL) {
        fprintf(gLog, "cannot load %s (%lu)\n", DBGENG_PATH, GetLastError());
        return 3;
    }
    Hr = Create(&IID_IDebugClient, (void **)&Client);
    if (FAILED(Hr)) {
        fprintf(gLog, "DebugCreate 0x%08lx\n", Hr);
        return 3;
    }
    Client->lpVtbl->QueryInterface(Client, &IID_IDebugControl, (void **)&Control);
    Client->lpVtbl->SetOutputCallbacks(Client, &gOut);
    Control->lpVtbl->Execute(Control, DEBUG_OUTCTL_ALL_CLIENTS,
                             ".symfix+ C:\\Users\\ilya\\code\\s10e-artifacts\\sym", DEBUG_EXECUTE_ECHO);
    if (InitCmds != NULL)
        Control->lpVtbl->AddEngineOptions(Control, DEBUG_ENGOPT_INITIAL_BREAK);
    Hr = Client->lpVtbl->AttachKernel(Client, DEBUG_ATTACH_KERNEL_CONNECTION, "net:port=50005,key=1.2.3.4");
    Note(SUCCEEDED(Hr) ? "AttachKernel ok - waiting for the target" : "AttachKernel FAILED");
    if (FAILED(Hr))
        return 4;

    Start = GetTickCount64();
    while (GetTickCount64() - Start < (ULONGLONG)TotalS * 1000) {
        ULONG Status = 0;

        Hr = Control->lpVtbl->WaitForEvent(Control, 0, 1000);
        Control->lpVtbl->GetExecutionStatus(Control, &Status);
        /* AttachKernel succeeds without a target; only the engine's own "Connected to ..." line means the phone
         * is there (the first m4kd1 run reported a connection that never happened). */
        if (!Connected && gSawConnect) {
            Connected = GetTickCount64();
            Note("target connected");
        }
        if (Hr == S_OK && Status == DEBUG_STATUS_BREAK) {
            ULONG Type = 0, Pid = 0, Tid = 0;
            char Desc[512] = "";
            ULONG DescLen = 0;
            BOOL Bugcheck;

            if (!Connected) {
                Connected = GetTickCount64();
                Note("target connected (first break)");
            }
            Control->lpVtbl->GetLastEventInformation(Control, &Type, &Pid, &Tid, NULL, 0, NULL, Desc, sizeof(Desc),
                                                     &DescLen);
            Bugcheck = strstr(Desc, "ugcheck") != NULL || strstr(Desc, "ug check") != NULL;
            Note(Desc[0] ? Desc : "break (no description)");
            if (FirstBreak && InitCmds != NULL && !Bugcheck) {
                FirstBreak = FALSE;
                RunFile(Control, InitCmds);
                Control->lpVtbl->SetExecutionStatus(Control, DEBUG_STATUS_GO);
                continue;
            }
            FirstBreak = FALSE;
            Diagnose(Control, Bugcheck);
            if (Bugcheck && Twrp) {
                ResetToTwrp(Control);
                break;
            }
            if (Bugcheck && Reboot) {
                Run(Control, ".reboot");
                Connected = 0;
            } else {
                Control->lpVtbl->SetExecutionStatus(Control, DEBUG_STATUS_GO);
            }
            continue;
        }
        if (Connected && !BrokeIn && GetTickCount64() - Connected > (ULONGLONG)BreakS * 1000) {
            BrokeIn = TRUE;
            Note("break-in timer expired - interrupting the target");
            Control->lpVtbl->SetInterrupt(Control, DEBUG_INTERRUPT_ACTIVE);
            if (Control->lpVtbl->WaitForEvent(Control, 0, 30000) == S_OK) {
                Diagnose(Control, FALSE);
                if (Twrp) {
                    ResetToTwrp(Control);
                    break;
                }
                if (Reboot) {
                    Run(Control, ".reboot");
                    Connected = 0;
                } else {
                    Control->lpVtbl->SetExecutionStatus(Control, DEBUG_STATUS_GO);
                }
            } else {
                Note("the target did not break in within 30 s");
            }
        }
    }
    Note("time limit - detaching");
    Client->lpVtbl->DetachProcesses(Client);
    Client->lpVtbl->EndSession(Client, DEBUG_END_ACTIVE_DETACH);
    fclose(gLog);
    return 0;
}
