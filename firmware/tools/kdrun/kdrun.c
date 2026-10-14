/*
 * kdrun.exe - unattended kernel-debugger session for the S10e (host x64, dbgeng from the Debugging Tools).
 * notes/m14-smp.md "M4 under KD": the M4 big cores hang/kill Windows early; a person is not always there to press
 * Ctrl+Break, so this does it:
 *
 *   kdrun <log> <total_s> <break_after_s> [reboot|twrp]
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
 * Diagnostics: !analyze -v (on a bugcheck), !running -it (every processor's current thread + stack), !prcb per CPU,
 * k on the current processor, lm, vertarget.
 */

#include <windows.h>
#include <stdio.h>
#include <initguid.h>
#include <dbgeng.h>

static FILE *gLog;

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
    BOOL Reboot, Twrp, BrokeIn = FALSE;
    HRESULT Hr;

    if (argc < 4) {
        fprintf(stderr, "usage: kdrun <log> <total_s> <break_after_s> [reboot]\n");
        return 1;
    }
    if (fopen_s(&gLog, argv[1], "a") != 0 || gLog == NULL)
        return 2;
    TotalS = strtoul(argv[2], NULL, 10);
    BreakS = strtoul(argv[3], NULL, 10);
    Reboot = argc > 4 && _stricmp(argv[4], "reboot") == 0;
    Twrp = argc > 4 && _stricmp(argv[4], "twrp") == 0;

    Hr = DebugCreate(&IID_IDebugClient, (void **)&Client);
    if (FAILED(Hr)) {
        fprintf(gLog, "DebugCreate 0x%08lx\n", Hr);
        return 3;
    }
    Client->lpVtbl->QueryInterface(Client, &IID_IDebugControl, (void **)&Control);
    Client->lpVtbl->SetOutputCallbacks(Client, &gOut);
    Control->lpVtbl->Execute(Control, DEBUG_OUTCTL_ALL_CLIENTS,
                             ".symfix+ C:\\Users\\ilya\\code\\s10e-artifacts\\sym", DEBUG_EXECUTE_ECHO);
    Hr = Client->lpVtbl->AttachKernel(Client, DEBUG_ATTACH_KERNEL_CONNECTION, "net:port=50005,key=1.2.3.4");
    Note(SUCCEEDED(Hr) ? "AttachKernel ok - waiting for the target" : "AttachKernel FAILED");
    if (FAILED(Hr))
        return 4;

    Start = GetTickCount64();
    while (GetTickCount64() - Start < (ULONGLONG)TotalS * 1000) {
        ULONG Status = 0;

        Hr = Control->lpVtbl->WaitForEvent(Control, 0, 1000);
        Control->lpVtbl->GetExecutionStatus(Control, &Status);
        if (!Connected && Hr != E_UNEXPECTED) {
            ULONG Class = 0, Qual = 0;
            if (SUCCEEDED(Control->lpVtbl->GetDebuggeeType(Control, &Class, &Qual)) && Class == DEBUG_CLASS_KERNEL &&
                Hr != S_FALSE) {
                Connected = GetTickCount64();
                Note("target connected");
            }
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
