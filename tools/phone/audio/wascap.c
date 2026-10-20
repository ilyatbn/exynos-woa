/*
 * wascap.exe SECONDS OUT.raw - record the default capture endpoint (WASAPI, shared mode, event-less polling) for
 * SECONDS and write the samples as interleaved 32-bit float (the shared-mode mix format), then print the endpoint
 * name and the mix format. For the phone microphone test (notes/d8-mic.md): analysed on the PC with
 * tools/phone/audio/micanalyze.py --float.
 * Build: tools/phone/audio/build-wascap.cmd (ARM64).
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdio.h>
#include <stdlib.h>

/* The SDK only declares these for C; local definitions (values from mmdeviceapi.h / audioclient.h). */
static const CLSID MyCLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID MyIID_IMMDeviceEnumerator = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID MyIID_IAudioClient = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID MyIID_IAudioCaptureClient = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

#define CHECK(hr, what) do { if (FAILED(hr)) { printf("%s failed 0x%08lx\n", what, (unsigned long)(hr)); return 1; } } while (0)

int wmain(int argc, wchar_t **argv)
{
    IMMDeviceEnumerator *En = NULL;
    IMMDevice           *Dev = NULL;
    IPropertyStore      *Props = NULL;
    IAudioClient        *Client = NULL;
    IAudioCaptureClient *Cap = NULL;
    WAVEFORMATEX        *Fmt = NULL;
    PROPVARIANT          Name;
    HRESULT              hr;
    FILE                *Out;
    double               Seconds;
    UINT64               Frames = 0, Wanted;
    DWORD                Silent = 0, Glitch = 0;

    if (argc < 3) {
        printf("usage: wascap SECONDS OUT.raw\n");
        return 2;
    }
    Seconds = _wtof(argv[1]);
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    CHECK(hr, "CoInitializeEx");
    hr = CoCreateInstance(&MyCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &MyIID_IMMDeviceEnumerator, (void **)&En);
    CHECK(hr, "MMDeviceEnumerator");
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(En, eCapture, eConsole, &Dev);
    CHECK(hr, "GetDefaultAudioEndpoint(eCapture)");
    PropVariantInit(&Name);
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(Dev, STGM_READ, &Props)) &&
        SUCCEEDED(IPropertyStore_GetValue(Props, &PKEY_Device_FriendlyName, &Name)) && Name.vt == VT_LPWSTR) {
        printf("endpoint: %ls\n", Name.pwszVal);
    }
    hr = IMMDevice_Activate(Dev, &MyIID_IAudioClient, CLSCTX_ALL, NULL, (void **)&Client);
    CHECK(hr, "Activate(IAudioClient)");
    hr = IAudioClient_GetMixFormat(Client, &Fmt);
    CHECK(hr, "GetMixFormat");
    printf("mix format: tag 0x%x, %u ch, %lu Hz, %u bit, block %u\n", Fmt->wFormatTag, Fmt->nChannels,
           (unsigned long)Fmt->nSamplesPerSec, Fmt->wBitsPerSample, Fmt->nBlockAlign);
    hr = IAudioClient_Initialize(Client, AUDCLNT_SHAREMODE_SHARED, 0, 10000000 /* 1 s buffer */, 0, Fmt, NULL);
    CHECK(hr, "Initialize");
    hr = IAudioClient_GetService(Client, &MyIID_IAudioCaptureClient, (void **)&Cap);
    CHECK(hr, "GetService(IAudioCaptureClient)");
    if (_wfopen_s(&Out, argv[2], L"wb") != 0) {
        printf("cannot open %ls\n", argv[2]);
        return 1;
    }
    Wanted = (UINT64)(Seconds * Fmt->nSamplesPerSec);
    hr = IAudioClient_Start(Client);
    CHECK(hr, "Start");
    while (Frames < Wanted) {
        UINT32 Packet = 0;

        Sleep(10);
        hr = IAudioCaptureClient_GetNextPacketSize(Cap, &Packet);
        CHECK(hr, "GetNextPacketSize");
        while (Packet != 0) {
            BYTE  *Data;
            UINT32 N;
            DWORD  Flags;

            hr = IAudioCaptureClient_GetBuffer(Cap, &Data, &N, &Flags, NULL, NULL);
            CHECK(hr, "GetBuffer");
            if (Flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                Silent++;
                {
                    BYTE *Z = calloc(N, Fmt->nBlockAlign);
                    fwrite(Z, Fmt->nBlockAlign, N, Out);
                    free(Z);
                }
            } else {
                fwrite(Data, Fmt->nBlockAlign, N, Out);
            }
            if (Flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                Glitch++;
            }
            Frames += N;
            hr = IAudioCaptureClient_ReleaseBuffer(Cap, N);
            CHECK(hr, "ReleaseBuffer");
            hr = IAudioCaptureClient_GetNextPacketSize(Cap, &Packet);
            CHECK(hr, "GetNextPacketSize");
        }
    }
    IAudioClient_Stop(Client);
    fclose(Out);
    printf("recorded %llu frames (%.2f s), silent packets %lu, discontinuities %lu\n", (unsigned long long)Frames,
           (double)Frames / Fmt->nSamplesPerSec, Silent, Glitch);
    return 0;
}
