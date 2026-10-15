/*
 * btspp.c - Bluetooth RFCOMM data test between the PC and the S10e (D4c, notes/bt-driver.md): proves ACL + L2CAP +
 * RFCOMM data both ways over the Exynos9820Bt transport, after pairing (tools/phone/btpair).
 *
 *   btspp server [seconds]           PC: listen on RFCOMM, publish the test service in SDP, echo everything back
 *   btspp client <name> [kib]        phone: find the paired device by name, connect to the test service by UUID,
 *                                     send <kib> KiB (default 1024) of a pattern, verify the echo, print throughput
 * Prints names, byte counts and timings only - never Bluetooth addresses. Build x64 (PC) and ARM64 (phone).
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>
#include <bluetoothapis.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bthprops.lib")

// {6A2E8C51-0B7D-4C3E-8F14-982AC0DE0B70} - exynos-woa RFCOMM echo test service
static const GUID kService = { 0x6a2e8c51, 0x0b7d, 0x4c3e, { 0x8f, 0x14, 0x98, 0x2a, 0xc0, 0xde, 0x0b, 0x70 } };

static unsigned char Pattern(unsigned i) { return (unsigned char)(i * 7u + (i >> 9)); }

static int Server(int seconds)
{
    SOCKET       s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM), c;
    SOCKADDR_BTH a = { 0 };
    int          len = sizeof(a);
    CSADDR_INFO  csa = { 0 };
    WSAQUERYSETW qs = { 0 };
    DWORD        timeout = (DWORD)seconds * 1000u;
    static char  buf[65536];

    if (s == INVALID_SOCKET)
    {
        printf("socket %d\n", WSAGetLastError());
        return 1;
    }
    a.addressFamily = AF_BTH;
    a.port = BT_PORT_ANY;
    if (bind(s, (SOCKADDR *)&a, sizeof(a)) || getsockname(s, (SOCKADDR *)&a, &len) || listen(s, 1))
    {
        printf("bind/listen %d\n", WSAGetLastError());
        return 1;
    }
    csa.LocalAddr.iSockaddrLength = sizeof(a);
    csa.LocalAddr.lpSockaddr = (SOCKADDR *)&a;
    csa.iSocketType = SOCK_STREAM;
    csa.iProtocol = BTHPROTO_RFCOMM;
    qs.dwSize = sizeof(qs);
    qs.lpszServiceInstanceName = (LPWSTR)L"exynos-woa RFCOMM echo";
    qs.lpServiceClassId = (LPGUID)&kService;
    qs.dwNameSpace = NS_BTH;
    qs.dwNumberOfCsAddrs = 1;
    qs.lpcsaBuffer = &csa;
    printf("RFCOMM channel %lu, SDP publish %d\n", (unsigned long)a.port, WSASetServiceW(&qs, RNRSERVICE_REGISTER, 0));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    printf("waiting %d s for a client\n", seconds);
    fflush(stdout);
    {
        fd_set         r;
        struct timeval tv = { seconds, 0 };
        FD_ZERO(&r);
        FD_SET(s, &r);
        if (select(0, &r, NULL, NULL, &tv) <= 0)
        {
            printf("no client\n");
            return 2;
        }
    }
    c = accept(s, NULL, NULL);
    if (c == INVALID_SOCKET)
    {
        printf("accept %d\n", WSAGetLastError());
        return 1;
    }
    printf("client connected\n");
    fflush(stdout);
    for (unsigned long long total = 0;;)
    {
        int n = recv(c, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            printf("client gone after %llu bytes echoed (recv %d, err %d)\n", total, n, WSAGetLastError());
            break;
        }
        for (int off = 0; off < n;)
        {
            int w = send(c, buf + off, n - off, 0);
            if (w <= 0)
                break;
            off += w;
        }
        total += (unsigned long long)n;
    }
    closesocket(c);
    WSASetServiceW(&qs, RNRSERVICE_DELETE, 0);
    closesocket(s);
    return 0;
}

static int FindPaired(const wchar_t *name, BTH_ADDR *addr)
{
    BLUETOOTH_DEVICE_SEARCH_PARAMS sp = { sizeof(sp) };
    BLUETOOTH_DEVICE_INFO          di = { sizeof(di) };
    HBLUETOOTH_DEVICE_FIND         f;

    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnConnected = TRUE;
    f = BluetoothFindFirstDevice(&sp, &di);
    if (!f)
        return 0;
    do
    {
        if (wcsstr(di.szName, name))
        {
            *addr = di.Address.ullLong;
            printf("paired device \"%ls\" (authenticated %d)\n", di.szName, di.fAuthenticated);
            BluetoothFindDeviceClose(f);
            return 1;
        }
    } while (BluetoothFindNextDevice(f, &di));
    BluetoothFindDeviceClose(f);
    return 0;
}

static int Client(const wchar_t *name, unsigned kib)
{
    SOCKADDR_BTH   a = { 0 };
    SOCKET         s;
    unsigned       total = kib * 1024u, sent = 0, got = 0, bad = 0;
    static char    out[16384], in[65536];
    LARGE_INTEGER  f, t0, t1;
    unsigned long  nb = 1;

    BTH_ADDR       peer = 0;
    if (!FindPaired(name, &peer))
    {
        printf("no paired device named like \"%ls\"\n", name);
        return 2;
    }
    a.btAddr = peer;   // SOCKADDR_BTH is packed: no pointer to its members
    a.addressFamily = AF_BTH;
    a.serviceClassId = kService;   // port 0: Winsock resolves the RFCOMM channel through SDP
    s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if (connect(s, (SOCKADDR *)&a, sizeof(a)))
    {
        printf("connect %d\n", WSAGetLastError());
        return 1;
    }
    QueryPerformanceCounter(&t1);
    printf("connected (SDP + RFCOMM) in %.0f ms\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    ioctlsocket(s, FIONBIO, &nb);
    QueryPerformanceCounter(&t0);
    while (got < total)
    {
        if (sent < total)
        {
            unsigned chunk = total - sent < sizeof(out) ? total - sent : (unsigned)sizeof(out);
            for (unsigned i = 0; i < chunk; i++)
                out[i] = (char)Pattern(sent + i);
            int w = send(s, out, (int)chunk, 0);
            if (w > 0)
                sent += (unsigned)w;
            else if (WSAGetLastError() != WSAEWOULDBLOCK)
            {
                printf("send %d\n", WSAGetLastError());
                break;
            }
        }
        int n = recv(s, in, sizeof(in), 0);
        if (n > 0)
        {
            for (int i = 0; i < n; i++)
                if ((unsigned char)in[i] != Pattern(got + (unsigned)i))
                    bad++;
            got += (unsigned)n;
        }
        else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
        {
            printf("recv %d err %d\n", n, WSAGetLastError());
            break;
        }
        else if (sent >= total)
            Sleep(1);
        QueryPerformanceCounter(&t1);
        if ((t1.QuadPart - t0.QuadPart) / f.QuadPart > 120)
        {
            printf("timeout\n");
            break;
        }
    }
    QueryPerformanceCounter(&t1);
    double sec = (t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    printf("sent %u B, echoed back %u B, %u mismatched bytes, %.2f s -> %.1f KiB/s each way\n", sent, got, bad, sec,
           sec > 0 ? got / 1024.0 / sec : 0.0);
    closesocket(s);
    return (got == total && bad == 0) ? 0 : 3;
}

int wmain(int argc, wchar_t **argv)
{
    WSADATA w;
    int     r;

    WSAStartup(MAKEWORD(2, 2), &w);
    if (argc > 1 && wcscmp(argv[1], L"server") == 0)
        r = Server(argc > 2 ? _wtoi(argv[2]) : 300);
    else if (argc > 2 && wcscmp(argv[1], L"client") == 0)
        r = Client(argv[2], argc > 3 ? (unsigned)_wtoi(argv[3]) : 1024u);
    else
    {
        printf("usage: btspp server [seconds] | btspp client <name> [kib]\n");
        r = 2;
    }
    WSACleanup();
    return r;
}
