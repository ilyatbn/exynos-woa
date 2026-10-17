/*
 * acrun.c - G17 V6d (notes/g17-dxva-mfc.md): run a command line inside an AppContainer (the sandbox packaged apps
 * such as Media Player and Photos run in) and relay its stdout/stderr, to test what the UMD can reach from there
 * (the MFC device interface). The container profile "exynos-woa.acrun" is created on first use; files the command
 * needs must grant read access to ALL APPLICATION PACKAGES (S-1-15-2-1).
 *
 *   acrun <command line...>
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <windows.h>
#include <userenv.h>
#include <sddl.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

int wmain(int argc, wchar_t **argv)
{
    static const wchar_t name[] = L"exynos-woa.acrun";
    PSID sid = NULL;
    HRESULT hr;
    WCHAR cmd[4096] = L"";
    SECURITY_CAPABILITIES sc = { 0 };
    STARTUPINFOEXW si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    SIZE_T attrSize = 0;
    HANDLE rd = NULL, wr = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    DWORD code = 1;

    if (argc < 2) {
        fwprintf(stderr, L"usage: acrun <command line...>\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            wcscat_s(cmd, ARRAYSIZE(cmd), L" ");
        wcscat_s(cmd, ARRAYSIZE(cmd), argv[i]);
    }
    hr = CreateAppContainerProfile(name, name, L"exynos-woa AppContainer test", NULL, 0, &sid);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        hr = DeriveAppContainerSidFromAppContainerName(name, &sid);
    if (FAILED(hr)) {
        fwprintf(stderr, L"AppContainer profile: 0x%08lx\n", hr);
        return 1;
    }
    {
        LPWSTR s = NULL;
        ConvertSidToStringSidW(sid, &s);
        wprintf(L"acrun: AppContainer %s\n", s ? s : L"?");
        LocalFree(s);
    }
    sc.AppContainerSid = sid;   /* no capabilities: the plain container a packaged app without device caps gets */

    if (!CreatePipe(&rd, &wr, &sa, 0))
        return 1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrSize);
    if (!si.lpAttributeList || !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &sc,
                                   sizeof(sc), NULL, NULL))
        return 1;
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = wr;
    si.StartupInfo.hStdError = wr;
    si.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
        fwprintf(stderr, L"CreateProcess in the AppContainer: %lu\n", GetLastError());
        return 1;
    }
    CloseHandle(wr);
    for (;;) {
        char buf[4096];
        DWORD got = 0;
        if (!ReadFile(rd, buf, sizeof(buf), &got, NULL) || got == 0)
            break;
        fwrite(buf, 1, got, stdout);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    wprintf(L"acrun: exit %lu\n", code);
    return (int)code;
}
