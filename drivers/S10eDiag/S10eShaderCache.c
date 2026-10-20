/*
 * s10eshadercache.exe - "Shader cache": list the Mali UMD's compiled-shader caches, one per game / app, and clear
 * one or all of them (user request 2026-10-20: "have an option to clear the cache per game using an app").
 *
 * The UMD (Mesa 0192, src/util/disk_cache_os.c Windows backend, notes/games-unity.md G33) keeps one directory per
 * executable: %LOCALAPPDATA%\MaliUMD\ShaderCache\<exe name>-<crc32 of the exe path>\, with app.txt = the full exe
 * path (UTF-8). Clearing deletes that directory; the next start of the game compiles its shaders again and refills
 * it. A running game keeps its cache index mapped, so a game must be closed before its cache is cleared (checked
 * against the running processes' image paths). Runs in the user's session, no admin; caches of other users are not
 * touched.
 */

#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#include <stdio.h>
#include <wchar.h>

#define ID_LIST      100
#define ID_CLEAR     101
#define ID_CLEARALL  102
#define ID_REFRESH   103
#define ID_STATUS    104
#define MAX_ENTRIES  256

struct Entry {
    wchar_t Dir[MAX_PATH];     // full directory path
    wchar_t Name[MAX_PATH];    // exe name (directory name without the -hash suffix)
    wchar_t Exe[MAX_PATH];     // app.txt, or empty
    ULONGLONG Bytes;           // compiled shaders (everything but the index)
    ULONGLONG IndexBytes;      // the fixed-size "index" file (2 MB: the cache's size counter + key table)
};

static struct Entry gEntries[MAX_ENTRIES];
static int gCount;
static wchar_t gRoot[MAX_PATH];
static HWND gList, gStatus;
static int gDpi = 96;

static int
Px(int Value)
{
    return MulDiv(Value, gDpi, 96);
}

// Bytes of the files under Dir; the top-level "index" file goes to *Index instead (when Index != NULL).
static ULONGLONG
DirBytes(const wchar_t *Dir, ULONGLONG *Index)
{
    wchar_t Pattern[MAX_PATH], Sub[MAX_PATH];
    WIN32_FIND_DATAW Data;
    HANDLE Find;
    ULONGLONG Total = 0;

    if (swprintf_s(Pattern, MAX_PATH, L"%s\\*", Dir) < 0) {
        return 0;
    }
    Find = FindFirstFileW(Pattern, &Data);
    if (Find == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if (wcscmp(Data.cFileName, L".") == 0 || wcscmp(Data.cFileName, L"..") == 0) {
            continue;
        }
        if (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (swprintf_s(Sub, MAX_PATH, L"%s\\%s", Dir, Data.cFileName) >= 0) {
                Total += DirBytes(Sub, NULL);
            }
        } else if (Index != NULL && _wcsicmp(Data.cFileName, L"index") == 0) {
            // the disk space it really takes: the UMD makes new index files sparse (Mesa 0198);
            // GetCompressedFileSizeW returns the allocated size of a sparse file
            DWORD High = 0, Low = INVALID_FILE_SIZE;
            if (swprintf_s(Sub, MAX_PATH, L"%s\\%s", Dir, Data.cFileName) >= 0) {
                Low = GetCompressedFileSizeW(Sub, &High);
            }
            if (Low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
                High = Data.nFileSizeHigh;
                Low = Data.nFileSizeLow;
            }
            *Index += ((ULONGLONG)High << 32) | Low;
        } else {
            Total += ((ULONGLONG)Data.nFileSizeHigh << 32) | Data.nFileSizeLow;
        }
    } while (FindNextFileW(Find, &Data));
    FindClose(Find);
    return Total;
}

// Deletes Dir and everything under it; returns FALSE if anything could not be deleted.
static BOOL
DeleteTree(const wchar_t *Dir)
{
    wchar_t Pattern[MAX_PATH], Sub[MAX_PATH];
    WIN32_FIND_DATAW Data;
    HANDLE Find;
    BOOL Ok = TRUE;

    if (swprintf_s(Pattern, MAX_PATH, L"%s\\*", Dir) < 0) {
        return FALSE;
    }
    Find = FindFirstFileW(Pattern, &Data);
    if (Find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(Data.cFileName, L".") == 0 || wcscmp(Data.cFileName, L"..") == 0) {
                continue;
            }
            if (swprintf_s(Sub, MAX_PATH, L"%s\\%s", Dir, Data.cFileName) < 0) {
                Ok = FALSE;
                continue;
            }
            if (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                Ok = DeleteTree(Sub) && Ok;
            } else {
                if (Data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
                    SetFileAttributesW(Sub, FILE_ATTRIBUTE_NORMAL);
                }
                Ok = DeleteFileW(Sub) && Ok;
            }
        } while (FindNextFileW(Find, &Data));
        FindClose(Find);
    }
    return RemoveDirectoryW(Dir) && Ok;
}

// TRUE if a process with this image path is running (a cache in use cannot be cleared consistently).
static BOOL
IsRunning(const wchar_t *Exe)
{
    DWORD Pids[4096], Bytes;
    BOOL Found = FALSE;

    if (Exe[0] == 0 || !EnumProcesses(Pids, sizeof(Pids), &Bytes)) {
        return FALSE;
    }
    for (DWORD i = 0; i < Bytes / sizeof(DWORD) && !Found; i++) {
        HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pids[i]);
        wchar_t Image[MAX_PATH];
        DWORD Size = MAX_PATH;

        if (Process == NULL) {
            continue;
        }
        if (QueryFullProcessImageNameW(Process, 0, Image, &Size) && _wcsicmp(Image, Exe) == 0) {
            Found = TRUE;
        }
        CloseHandle(Process);
    }
    return Found;
}

static void
ReadAppTxt(struct Entry *E)
{
    wchar_t Path[MAX_PATH];
    char Utf8[MAX_PATH * 3];
    HANDLE File;
    DWORD Read = 0;

    E->Exe[0] = 0;
    if (swprintf_s(Path, MAX_PATH, L"%s\\app.txt", E->Dir) < 0) {
        return;
    }
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (File == INVALID_HANDLE_VALUE) {
        return;
    }
    if (ReadFile(File, Utf8, sizeof(Utf8) - 1, &Read, NULL)) {
        Utf8[Read] = 0;
        if (MultiByteToWideChar(CP_UTF8, 0, Utf8, -1, E->Exe, MAX_PATH) == 0) {
            E->Exe[0] = 0;
        }
    }
    CloseHandle(File);
}

static void
FormatBytes(ULONGLONG Bytes, wchar_t *Text, size_t Size)
{
    if (Bytes >= 1024ull * 1024) {
        swprintf_s(Text, Size, L"%.1f MB", Bytes / (1024.0 * 1024.0));
    } else {
        swprintf_s(Text, Size, L"%llu KB", (Bytes + 1023) / 1024);
    }
}

static void
Refresh(void)
{
    wchar_t Pattern[MAX_PATH], Text[64];
    WIN32_FIND_DATAW Data;
    HANDLE Find;
    ULONGLONG Total = 0, TotalIndex = 0;

    ListView_DeleteAllItems(gList);
    gCount = 0;
    if (swprintf_s(Pattern, MAX_PATH, L"%s\\*", gRoot) >= 0 &&
        (Find = FindFirstFileW(Pattern, &Data)) != INVALID_HANDLE_VALUE) {
        do {
            struct Entry *E = &gEntries[gCount];
            wchar_t *Dash;

            if (!(Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || Data.cFileName[0] == L'.') {
                continue;
            }
            if (swprintf_s(E->Dir, MAX_PATH, L"%s\\%s", gRoot, Data.cFileName) < 0) {
                continue;
            }
            wcscpy_s(E->Name, MAX_PATH, Data.cFileName);
            Dash = wcsrchr(E->Name, L'-');
            if (Dash != NULL) {
                *Dash = 0;   // "<exe>-<crc32>" -> "<exe>"
            }
            ReadAppTxt(E);
            E->IndexBytes = 0;
            E->Bytes = DirBytes(E->Dir, &E->IndexBytes);
            Total += E->Bytes;
            TotalIndex += E->IndexBytes;
            gCount++;
        } while (gCount < MAX_ENTRIES && FindNextFileW(Find, &Data));
        FindClose(Find);
    }

    for (int i = 0; i < gCount; i++) {
        LVITEMW Item = { 0 };

        Item.mask = LVIF_TEXT | LVIF_PARAM;
        Item.iItem = i;
        Item.pszText = gEntries[i].Name;
        Item.lParam = i;
        ListView_InsertItem(gList, &Item);
        FormatBytes(gEntries[i].Bytes, Text, 64);
        ListView_SetItemText(gList, i, 1, Text);
        ListView_SetItemText(gList, i, 2, gEntries[i].Exe[0] ? gEntries[i].Exe : L"?");
    }
    FormatBytes(Total, Text, 64);
    {
        wchar_t Status[160], Index[64];
        FormatBytes(TotalIndex, Index, 64);
        swprintf_s(Status, 160, L"%d cache%s, %s of shaders (+ %s of index files)", gCount, gCount == 1 ? L"" : L"s",
                   Text, Index);
        SetWindowTextW(gStatus, Status);
    }
}

// Clears one entry; returns 0 ok, 1 game running, 2 delete failed.
static int
ClearEntry(const struct Entry *E)
{
    if (IsRunning(E->Exe)) {
        return 1;
    }
    return DeleteTree(E->Dir) ? 0 : 2;
}

static void
ClearSelected(HWND Window)
{
    int Sel = ListView_GetNextItem(gList, -1, LVNI_SELECTED);
    wchar_t Text[MAX_PATH * 2];
    int Result;

    if (Sel < 0 || Sel >= gCount) {
        MessageBoxW(Window, L"Select a game first.", L"Shader cache", MB_ICONINFORMATION);
        return;
    }
    swprintf_s(Text, ARRAYSIZE(Text), L"Clear the shader cache of %s?\n\nIts next start compiles the shaders again "
               L"(slower loading once).", gEntries[Sel].Name);
    if (MessageBoxW(Window, Text, L"Shader cache", MB_ICONQUESTION | MB_YESNO) != IDYES) {
        return;
    }
    Result = ClearEntry(&gEntries[Sel]);
    if (Result == 1) {
        swprintf_s(Text, ARRAYSIZE(Text), L"%s is running. Close it first.", gEntries[Sel].Name);
        MessageBoxW(Window, Text, L"Shader cache", MB_ICONWARNING);
    } else if (Result == 2) {
        MessageBoxW(Window, L"Some files could not be deleted (in use?).", L"Shader cache", MB_ICONWARNING);
    }
    Refresh();
}

static void
ClearAll(HWND Window)
{
    int Running = 0, Failed = 0;
    wchar_t Text[160];

    if (gCount == 0 || MessageBoxW(Window, L"Clear the shader caches of all games?", L"Shader cache",
                                   MB_ICONQUESTION | MB_YESNO) != IDYES) {
        return;
    }
    for (int i = 0; i < gCount; i++) {
        int Result = ClearEntry(&gEntries[i]);
        Running += Result == 1;
        Failed += Result == 2;
    }
    if (Running || Failed) {
        swprintf_s(Text, 160, L"Skipped %d running game%s; %d could not be fully deleted.", Running,
                   Running == 1 ? L"" : L"s", Failed);
        MessageBoxW(Window, Text, L"Shader cache", MB_ICONWARNING);
    }
    Refresh();
}

static BOOL CALLBACK
SetChildFont(HWND Child, LPARAM Font)
{
    SendMessageW(Child, WM_SETFONT, (WPARAM)Font, TRUE);
    return TRUE;
}

static void
Layout(HWND Window)
{
    RECT Client;
    int W, H, Button = Px(36), Gap = Px(10);

    GetClientRect(Window, &Client);
    W = Client.right - 2 * Gap;
    H = Client.bottom;
    MoveWindow(gList, Gap, Gap, W, H - 3 * Gap - Button - Px(24), TRUE);
    MoveWindow(gStatus, Gap, H - 2 * Gap - Button - Px(22), W, Px(22), TRUE);
    MoveWindow(GetDlgItem(Window, ID_CLEAR), Gap, H - Gap - Button, Px(140), Button, TRUE);
    MoveWindow(GetDlgItem(Window, ID_CLEARALL), Gap + Px(150), H - Gap - Button, Px(110), Button, TRUE);
    MoveWindow(GetDlgItem(Window, ID_REFRESH), Gap + Px(270), H - Gap - Button, Px(90), Button, TRUE);
}

static LRESULT CALLBACK
WndProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
    switch (Message) {
    case WM_CREATE: {
        HFONT Font;
        NONCLIENTMETRICSW Metrics = { sizeof(Metrics) };
        LVCOLUMNW Column = { 0 };

        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(Metrics), &Metrics, 0);
        Font = CreateFontIndirectW(&Metrics.lfMessageFont);
        gList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                0, 0, 10, 10, Window, (HMENU)ID_LIST, NULL, NULL);
        ListView_SetExtendedListViewStyle(gList, LVS_EX_FULLROWSELECT);
        Column.mask = LVCF_TEXT | LVCF_WIDTH;
        Column.pszText = L"Game";
        Column.cx = Px(170);
        ListView_InsertColumn(gList, 0, &Column);
        Column.pszText = L"Shaders";
        Column.cx = Px(80);
        ListView_InsertColumn(gList, 1, &Column);
        Column.pszText = L"Program";
        Column.cx = Px(420);
        ListView_InsertColumn(gList, 2, &Column);
        gStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, Window, (HMENU)ID_STATUS,
                                NULL, NULL);
        CreateWindowW(L"BUTTON", L"Clear selected", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, Window,
                      (HMENU)ID_CLEAR, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Clear all", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, Window,
                      (HMENU)ID_CLEARALL, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, Window,
                      (HMENU)ID_REFRESH, NULL, NULL);
        EnumChildWindows(Window, SetChildFont, (LPARAM)Font);
        Layout(Window);
        Refresh();
        return 0;
    }
    case WM_SIZE:
        Layout(Window);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(WParam)) {
        case ID_CLEAR:
            ClearSelected(Window);
            break;
        case ID_CLEARALL:
            ClearAll(Window);
            break;
        case ID_REFRESH:
            Refresh();
            break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(Window, Message, WParam, LParam);
}

int WINAPI
wWinMain(HINSTANCE Instance, HINSTANCE Previous, PWSTR CommandLine, int Show)
{
    INITCOMMONCONTROLSEX Controls = { sizeof(Controls), ICC_LISTVIEW_CLASSES };
    WNDCLASSW Class = { 0 };
    wchar_t Local[MAX_PATH];
    HWND Window;
    HDC Screen;
    MSG Message;
    DWORD Length;

    UNREFERENCED_PARAMETER(Previous);
    UNREFERENCED_PARAMETER(CommandLine);
    SetProcessDPIAware();
    Screen = GetDC(NULL);
    gDpi = GetDeviceCaps(Screen, LOGPIXELSX);
    ReleaseDC(NULL, Screen);
    InitCommonControlsEx(&Controls);

    // Same root as the UMD (disk_cache_os.c WIN_CACHE_ROOT under %LOCALAPPDATA%).
    Length = GetEnvironmentVariableW(L"LOCALAPPDATA", Local, MAX_PATH);
    if (Length == 0 || Length >= MAX_PATH ||
        swprintf_s(gRoot, MAX_PATH, L"%s\\MaliUMD\\ShaderCache", Local) < 0) {
        MessageBoxW(NULL, L"LOCALAPPDATA is not set.", L"Shader cache", MB_ICONERROR);
        return 1;
    }

    Class.lpfnWndProc = WndProc;
    Class.hInstance = Instance;
    Class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    Class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    Class.lpszClassName = L"S10eShaderCache";
    RegisterClassW(&Class);

    Window = CreateWindowW(L"S10eShaderCache", L"Shader cache", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, Px(700), Px(420), NULL, NULL, Instance, NULL);
    ShowWindow(Window, Show);
    while (GetMessageW(&Message, NULL, 0, 0) > 0) {
        TranslateMessage(&Message);
        DispatchMessageW(&Message);
    }
    return 0;
}
