/*
 * S10eIddView - G18 remote-monitor PoC viewer server (notes/g18-remote-monitors-research.md, PoC log).
 * Reads the frames drivers/S10eIdd publishes in the shared section "Global\S10eIddFrame" (header + BGRA rows, seqlock)
 * and serves them over HTTP to any browser on the LAN:
 *   /           a page that shows the stream
 *   /stream     MJPEG (multipart/x-mixed-replace), ~10 frames/s
 *   /frame.jpg  one JPEG
 * View only (no input back). usage: S10eIddView [PORT=8090] [SECONDS=0 (forever)] [QUALITY=0.75]
 * SPDX-License-Identifier: GPL-2.0
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

struct FrameHdr { UINT32 Magic, Width, Height, Pitch; volatile LONG Seq; UINT32 Reserved[3]; };
static const UINT32 kMagic = 'S10F';
static float g_Quality = 0.75f;

/* Copy the newest complete frame out of the section (seqlock); false if none yet. */
static bool GrabFrame(std::vector<BYTE>& Pixels, UINT& W, UINT& H, UINT& Pitch, LONG& Seq)
{
    HANDLE Map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\S10eIddFrame");
    if (!Map) return false;
    const BYTE* View = static_cast<const BYTE*>(MapViewOfFile(Map, FILE_MAP_READ, 0, 0, 0));
    bool Ok = false;
    if (View) {
        const FrameHdr* Hdr = reinterpret_cast<const FrameHdr*>(View);
        for (int Try = 0; Try < 5 && !Ok; Try++) {
            LONG S1 = Hdr->Seq;
            if ((S1 & 1) || Hdr->Magic != kMagic || Hdr->Width == 0) { Sleep(5); continue; }
            W = Hdr->Width; H = Hdr->Height; Pitch = Hdr->Pitch;
            Pixels.resize(SIZE_T(Pitch) * H);
            memcpy(Pixels.data(), View + sizeof(FrameHdr), Pixels.size());
            MemoryBarrier();
            if (Hdr->Seq == S1) { Ok = true; Seq = S1; }
        }
        UnmapViewOfFile(View);
    }
    CloseHandle(Map);
    return Ok;
}

static bool EncodeJpeg(IWICImagingFactory* Wic, const std::vector<BYTE>& Pixels, UINT W, UINT H, UINT Pitch,
                       std::vector<BYTE>& Jpeg)
{
    IWICBitmap* Bmp = nullptr; IStream* St = nullptr; IWICBitmapEncoder* Enc = nullptr;
    IWICBitmapFrameEncode* Fr = nullptr; IPropertyBag2* Props = nullptr;
    bool Ok = false;
    if (FAILED(Wic->CreateBitmapFromMemory(W, H, GUID_WICPixelFormat32bppBGRA, Pitch, UINT(Pixels.size()),
                                           const_cast<BYTE*>(Pixels.data()), &Bmp))) goto out;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &St))) goto out;
    if (FAILED(Wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &Enc)) ||
        FAILED(Enc->Initialize(St, WICBitmapEncoderNoCache)) || FAILED(Enc->CreateNewFrame(&Fr, &Props))) goto out;
    {
        PROPBAG2 Opt = {}; Opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT V; VariantInit(&V); V.vt = VT_R4; V.fltVal = g_Quality;
        Props->Write(1, &Opt, &V);
    }
    if (FAILED(Fr->Initialize(Props)) || FAILED(Fr->WriteSource(Bmp, nullptr)) || FAILED(Fr->Commit()) ||
        FAILED(Enc->Commit())) goto out;
    {
        HGLOBAL Mem = nullptr;
        if (FAILED(GetHGlobalFromStream(St, &Mem))) goto out;
        STATSTG Stat; St->Stat(&Stat, STATFLAG_NONAME);
        const BYTE* P = static_cast<const BYTE*>(GlobalLock(Mem));
        Jpeg.assign(P, P + Stat.cbSize.LowPart);
        GlobalUnlock(Mem);
        Ok = true;
    }
out:
    if (Props) Props->Release(); if (Fr) Fr->Release(); if (Enc) Enc->Release(); if (St) St->Release(); if (Bmp) Bmp->Release();
    return Ok;
}

static bool SendAll(SOCKET S, const void* Data, int Len)
{
    const char* P = static_cast<const char*>(Data);
    while (Len > 0) { int N = send(S, P, Len, 0); if (N <= 0) return false; P += N; Len -= N; }
    return true;
}
static bool SendStr(SOCKET S, const std::string& Str) { return SendAll(S, Str.data(), int(Str.size())); }

static DWORD WINAPI Client(LPVOID Arg)
{
    SOCKET S = SOCKET(reinterpret_cast<UINT_PTR>(Arg));
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* Wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Wic));
    char Req[2048] = {};
    int N = recv(S, Req, sizeof(Req) - 1, 0);
    std::string R(Req, N > 0 ? N : 0);
    std::vector<BYTE> Pixels, Jpeg;
    UINT W = 0, H = 0, Pitch = 0; LONG Seq = 0, LastSeq = -1;
    if (R.rfind("GET /stream", 0) == 0 && Wic) {
        SendStr(S, "HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\nConnection: close\r\n"
                   "Content-Type: multipart/x-mixed-replace; boundary=s10eframe\r\n\r\n");
        for (;;) {
            if (GrabFrame(Pixels, W, H, Pitch, Seq) && Seq != LastSeq && EncodeJpeg(Wic, Pixels, W, H, Pitch, Jpeg)) {
                LastSeq = Seq;
                char Part[160];
                sprintf_s(Part, "--s10eframe\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", Jpeg.size());
                if (!SendStr(S, Part) || !SendAll(S, Jpeg.data(), int(Jpeg.size())) || !SendStr(S, "\r\n")) break;
            }
            Sleep(100);
        }
    } else if (R.rfind("GET /frame.jpg", 0) == 0 && Wic) {
        if (GrabFrame(Pixels, W, H, Pitch, Seq) && EncodeJpeg(Wic, Pixels, W, H, Pitch, Jpeg)) {
            char Hdr[160];
            sprintf_s(Hdr, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", Jpeg.size());
            SendStr(S, Hdr); SendAll(S, Jpeg.data(), int(Jpeg.size()));
        } else {
            SendStr(S, "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\nno frame yet (is the S10eIdd monitor on?)\r\n");
        }
    } else {
        std::string Body = "<!doctype html><html><head><meta name=viewport content='width=device-width'><title>S10e "
                           "remote monitor</title><style>body{margin:0;background:#111}img{width:100%;height:auto;display:block}"
                           "</style></head><body><img src='/stream' alt='waiting for frames'></body></html>";
        SendStr(S, "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + std::to_string(Body.size()) +
                   "\r\nConnection: close\r\n\r\n" + Body);
    }
    closesocket(S);
    if (Wic) Wic->Release();
    CoUninitialize();
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    int Port = argc > 1 ? atoi(argv[1]) : 8090, Seconds = argc > 2 ? atoi(argv[2]) : 0;
    if (argc > 3) g_Quality = float(atof(argv[3]));
    WSADATA Wsa;
    WSAStartup(MAKEWORD(2, 2), &Wsa);
    SOCKET L = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in A = {}; A.sin_family = AF_INET; A.sin_port = htons(USHORT(Port)); A.sin_addr.s_addr = INADDR_ANY;
    if (L == INVALID_SOCKET || bind(L, reinterpret_cast<sockaddr*>(&A), sizeof(A)) || listen(L, 8)) {
        printf("listen on %d failed: %d\n", Port, WSAGetLastError());
        return 1;
    }
    printf("S10eIddView on port %d (%s)\n", Port, Seconds ? "timed" : "until killed");
    ULONGLONG T0 = GetTickCount64();
    for (;;) {
        if (Seconds && GetTickCount64() - T0 > ULONGLONG(Seconds) * 1000) break;
        fd_set Fds; FD_ZERO(&Fds); FD_SET(L, &Fds);
        timeval Tv = { 1, 0 };
        if (select(0, &Fds, nullptr, nullptr, &Tv) <= 0) continue;
        SOCKET C = accept(L, nullptr, nullptr);
        if (C == INVALID_SOCKET) continue;
        HANDLE T = CreateThread(nullptr, 0, Client, reinterpret_cast<LPVOID>(UINT_PTR(C)), 0, nullptr);
        if (T) CloseHandle(T); else closesocket(C);
    }
    closesocket(L);
    WSACleanup();
    return 0;
}
