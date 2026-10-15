/*
 * Mft.cpp - Exynos9820 MFC H.264 decoder MFT (D10 V4, notes/d10-mfc-decode-flow.md "V4"). A synchronous Media
 * Foundation transform, H.264 (Annex B, or length-prefixed samples from an MP4 source) -> NV12, on top of the
 * Exynos9820Mfc IOCTLs (drivers/Exynos9820Mfc/MfcIoctl.h): one IOCTL_MFC_DECODE per access unit, pictures come back
 * in display order, so output timestamps are the smallest queued input timestamps.
 *
 * Registration (DllRegisterServer): CLSID + MFTRegister in MFT_CATEGORY_VIDEO_DECODER. Making it the decoder
 * Windows picks is a separate, reversible step (install.cmd: the H.264 entry of the Media Foundation preferred list).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <cfgmgr32.h>
#include <winioctl.h>
#include <new>
#include <vector>
#include <algorithm>
#include "..\Exynos9820Mfc\MfcIoctl.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "cfgmgr32.lib")

// {6A2E8C51-0B7D-4C3E-8F14-982AC0DE0D11}
DEFINE_GUID(CLSID_Exynos9820MfcH264, 0x6a2e8c51, 0x0b7d, 0x4c3e, 0x8f, 0x14, 0x98, 0x2a, 0xc0, 0xde, 0x0d, 0x11);

static HMODULE       g_Module;
static volatile LONG g_Objects;
static volatile LONG g_Locks;

static void Trace(const char *fmt, ...)
{
    char    buf[256];
    va_list ap;

    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}

/** Append a NAL unit / Annex-B data, converting length-prefixed (avcC style) input when LengthSize != 0. **/
static void AppendAnnexB(std::vector<BYTE> &out, const BYTE *p, DWORD n, UINT32 lengthSize)
{
    static const BYTE sc[4] = { 0, 0, 0, 1 };

    if (lengthSize == 0)
    {
        out.insert(out.end(), p, p + n);
        return;
    }

    while (n >= lengthSize)
    {
        DWORD len = 0;
        for (UINT32 i = 0; i < lengthSize; i++)
            len = (len << 8) | p[i];
        p += lengthSize;
        n -= lengthSize;
        if (len > n)
            len = n;
        out.insert(out.end(), sc, sc + 4);
        out.insert(out.end(), p, p + len);
        p += len;
        n -= len;
    }
}

/** avcC (AVCDecoderConfigurationRecord) -> Annex-B SPS/PPS; returns the NAL length size, 0 if not avcC. **/
static UINT32 AvcCToAnnexB(const BYTE *p, UINT32 n, std::vector<BYTE> &out)
{
    static const BYTE sc[4] = { 0, 0, 0, 1 };
    UINT32            lengthSize, count, i, off = 6;

    if (n < 7 || p[0] != 1)
        return 0;
    lengthSize = (p[4] & 3u) + 1u;
    for (int kind = 0; kind < 2; kind++)
    {
        if (off >= n)
            break;
        count = kind == 0 ? (p[5] & 0x1Fu) : p[off++];
        for (i = 0; i < count && off + 2 <= n; i++)
        {
            UINT32 len = (p[off] << 8) | p[off + 1];
            off += 2;
            if (off + len > n)
                return lengthSize;
            out.insert(out.end(), sc, sc + 4);
            out.insert(out.end(), p + off, p + off + len);
            off += len;
        }
    }
    return lengthSize;
}

/** Remember the latest SPS (7) / PPS (8) NAL units of an Annex-B buffer (replayed after a flush or a drain). **/
static void CaptureParameterSets(const BYTE *p, size_t n, std::vector<BYTE> &sps, std::vector<BYTE> &pps)
{
    size_t i = 0;

    while (i + 3 < n)
    {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
        {
            size_t start = i + 3, end = start;
            while (end + 2 < n && !(p[end] == 0 && p[end + 1] == 0 && (p[end + 2] == 1 || p[end + 2] == 0)))
                end++;
            if (end + 2 >= n)
                end = n;
            BYTE type = p[start] & 0x1F;
            if (type == 7 || type == 8)
            {
                std::vector<BYTE> &dst = type == 7 ? sps : pps;
                static const BYTE  sc[4] = { 0, 0, 0, 1 };
                dst.assign(sc, sc + 4);
                dst.insert(dst.end(), p + start, p + end);
            }
            i = end;
        }
        else
            i++;
    }
}

class CMfcDecoder : public IMFTransform
{
public:
    CMfcDecoder() { InitializeCriticalSection(&m_Lock); InterlockedIncrement(&g_Objects); }
    ~CMfcDecoder()
    {
        CloseSession();
        if (m_Dev != INVALID_HANDLE_VALUE)
            CloseHandle(m_Dev);
        if (m_In) m_In->Release();
        if (m_Out) m_Out->Release();
        if (m_Attr) m_Attr->Release();
        DeleteCriticalSection(&m_Lock);
        InterlockedDecrement(&g_Objects);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (ppv == nullptr)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFTransform)
        {
            *ppv = static_cast<IMFTransform *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = InterlockedDecrement(&m_Ref);
        if (r == 0)
            delete this;
        return r;
    }

    // IMFTransform: streams
    STDMETHODIMP GetStreamLimits(DWORD *a, DWORD *b, DWORD *c, DWORD *d) override
    {
        if (!a || !b || !c || !d) return E_POINTER;
        *a = *b = *c = *d = 1;
        return S_OK;
    }
    STDMETHODIMP GetStreamCount(DWORD *in, DWORD *out) override
    {
        if (!in || !out) return E_POINTER;
        *in = *out = 1;
        return S_OK;
    }
    STDMETHODIMP GetStreamIDs(DWORD, DWORD *, DWORD, DWORD *) override { return E_NOTIMPL; }
    STDMETHODIMP GetInputStreamInfo(DWORD id, MFT_INPUT_STREAM_INFO *info) override
    {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!info) return E_POINTER;
        ZeroMemory(info, sizeof(*info));
        info->dwFlags = MFT_INPUT_STREAM_WHOLE_SAMPLES | MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER;
        return S_OK;
    }
    STDMETHODIMP GetOutputStreamInfo(DWORD id, MFT_OUTPUT_STREAM_INFO *info) override
    {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!info) return E_POINTER;
        EnterCriticalSection(&m_Lock);
        info->dwFlags = MFT_OUTPUT_STREAM_WHOLE_SAMPLES | MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER |
                        MFT_OUTPUT_STREAM_FIXED_SAMPLE_SIZE | MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;
        info->cbSize = m_DispW * m_DispH * 3 / 2;
        info->cbAlignment = 0;
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }
    STDMETHODIMP GetAttributes(IMFAttributes **attr) override
    {
        if (!attr) return E_POINTER;
        EnterCriticalSection(&m_Lock);
        HRESULT hr = S_OK;
        if (!m_Attr)
        {
            hr = MFCreateAttributes(&m_Attr, 2);
            if (SUCCEEDED(hr))
                m_Attr->SetString(MFT_FRIENDLY_NAME_Attribute, L"Exynos9820 MFC H.264 Decoder (hardware)");
        }
        if (SUCCEEDED(hr))
        {
            *attr = m_Attr;
            m_Attr->AddRef();
        }
        LeaveCriticalSection(&m_Lock);
        return hr;
    }
    STDMETHODIMP GetInputStreamAttributes(DWORD, IMFAttributes **) override { return E_NOTIMPL; }
    STDMETHODIMP GetOutputStreamAttributes(DWORD, IMFAttributes **) override { return E_NOTIMPL; }
    STDMETHODIMP DeleteInputStream(DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP AddInputStreams(DWORD, DWORD *) override { return E_NOTIMPL; }

    // IMFTransform: media types
    STDMETHODIMP GetInputAvailableType(DWORD id, DWORD index, IMFMediaType **type) override
    {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!type) return E_POINTER;
        if (index != 0) return MF_E_NO_MORE_TYPES;
        HRESULT hr = MFCreateMediaType(type);
        if (SUCCEEDED(hr)) hr = (*type)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = (*type)->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        return hr;
    }
    STDMETHODIMP GetOutputAvailableType(DWORD id, DWORD index, IMFMediaType **type) override
    {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!type) return E_POINTER;
        EnterCriticalSection(&m_Lock);
        HRESULT hr = !m_In ? MF_E_TRANSFORM_TYPE_NOT_SET : (index != 0 ? MF_E_NO_MORE_TYPES : MakeOutputType(type));
        LeaveCriticalSection(&m_Lock);
        return hr;
    }
    STDMETHODIMP SetInputType(DWORD id, IMFMediaType *type, DWORD flags) override
    {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (flags & ~MFT_SET_TYPE_TEST_ONLY) return E_INVALIDARG;
        EnterCriticalSection(&m_Lock);
        HRESULT hr = S_OK;
        if (type)
        {
            GUID major = {}, sub = {};
            type->GetGUID(MF_MT_MAJOR_TYPE, &major);
            type->GetGUID(MF_MT_SUBTYPE, &sub);
            if (major != MFMediaType_Video || sub != MFVideoFormat_H264)
                hr = MF_E_INVALIDMEDIATYPE;
            else if (!ProbeDevice())
                hr = MF_E_INVALIDMEDIATYPE;   // no MFC (driver missing / not started): let the next decoder win
        }
        if (SUCCEEDED(hr) && !(flags & MFT_SET_TYPE_TEST_ONLY))
        {
            if (m_In) m_In->Release();
            m_In = type;
            if (m_In)
            {
                m_In->AddRef();
                UINT32 w = 0, h = 0;
                if (SUCCEEDED(MFGetAttributeSize(m_In, MF_MT_FRAME_SIZE, &w, &h)) && w && h && !m_HeaderSeen)
                {
                    m_DispW = w;
                    m_DispH = h;
                }
                ReadSequenceHeader();
            }
        }
        LeaveCriticalSection(&m_Lock);
        return hr;
    }
    STDMETHODIMP SetOutputType(DWORD id, IMFMediaType *type, DWORD flags) override
    {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (flags & ~MFT_SET_TYPE_TEST_ONLY) return E_INVALIDARG;
        EnterCriticalSection(&m_Lock);
        HRESULT hr = S_OK;
        if (type)
        {
            GUID sub = {};
            UINT32 w = 0, h = 0;
            type->GetGUID(MF_MT_SUBTYPE, &sub);
            if (!m_In)
                hr = MF_E_TRANSFORM_TYPE_NOT_SET;
            else if (sub != MFVideoFormat_NV12)
                hr = MF_E_INVALIDMEDIATYPE;
            else if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) && m_DispW && (w != m_DispW || h != m_DispH))
                hr = MF_E_INVALIDMEDIATYPE;
        }
        if (SUCCEEDED(hr) && !(flags & MFT_SET_TYPE_TEST_ONLY))
        {
            if (m_Out) m_Out->Release();
            m_Out = type;
            if (m_Out) m_Out->AddRef();
            m_FormatChange = false;
        }
        LeaveCriticalSection(&m_Lock);
        return hr;
    }
    STDMETHODIMP GetInputCurrentType(DWORD id, IMFMediaType **type) override
    {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!type) return E_POINTER;
        EnterCriticalSection(&m_Lock);
        HRESULT hr = m_In ? (m_In->AddRef(), *type = m_In, S_OK) : MF_E_TRANSFORM_TYPE_NOT_SET;
        LeaveCriticalSection(&m_Lock);
        return hr;
    }
    STDMETHODIMP GetOutputCurrentType(DWORD id, IMFMediaType **type) override
    {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!type) return E_POINTER;
        EnterCriticalSection(&m_Lock);
        HRESULT hr = m_Out ? (m_Out->AddRef(), *type = m_Out, S_OK) : MF_E_TRANSFORM_TYPE_NOT_SET;
        LeaveCriticalSection(&m_Lock);
        return hr;
    }
    STDMETHODIMP GetInputStatus(DWORD id, DWORD *flags) override
    {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!flags) return E_POINTER;
        EnterCriticalSection(&m_Lock);
        *flags = m_HavePending ? 0 : MFT_INPUT_STATUS_ACCEPT_DATA;
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }
    STDMETHODIMP GetOutputStatus(DWORD *) override { return E_NOTIMPL; }
    STDMETHODIMP SetOutputBounds(LONGLONG, LONGLONG) override { return E_NOTIMPL; }
    STDMETHODIMP ProcessEvent(DWORD, IMFMediaEvent *) override { return E_NOTIMPL; }

    STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE msg, ULONG_PTR) override
    {
        EnterCriticalSection(&m_Lock);
        switch (msg)
        {
        case MFT_MESSAGE_COMMAND_FLUSH:
            m_Pending.clear();
            m_HavePending = false;
            m_Pts.clear();
            m_Draining = false;
            CloseSession();
            break;
        case MFT_MESSAGE_COMMAND_DRAIN:
            m_Draining = true;
            break;
        default:
            break;
        }
        LeaveCriticalSection(&m_Lock);
        return S_OK;
    }

    STDMETHODIMP ProcessInput(DWORD id, IMFSample *sample, DWORD flags) override
    {
        if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
        if (!sample || flags) return E_INVALIDARG;
        EnterCriticalSection(&m_Lock);
        HRESULT         hr = S_OK;
        IMFMediaBuffer *buf = nullptr;
        BYTE           *p = nullptr;
        DWORD           n = 0;
        if (!m_In)
            hr = MF_E_TRANSFORM_TYPE_NOT_SET;
        else if (m_HavePending)
            hr = MF_E_NOTACCEPTING;
        if (SUCCEEDED(hr)) hr = sample->ConvertToContiguousBuffer(&buf);
        if (SUCCEEDED(hr)) hr = buf->Lock(&p, nullptr, &n);
        if (SUCCEEDED(hr))
        {
            m_Pending.clear();
            m_PendingOff = 0;
            if (!m_SessionOpen)                       // a new session needs the parameter sets first
            {
                m_Pending.insert(m_Pending.end(), m_Sps.begin(), m_Sps.end());
                m_Pending.insert(m_Pending.end(), m_Pps.begin(), m_Pps.end());
            }
            UINT32 ls = m_LengthSize;
            if (ls && n >= 4 && ((p[0] == 0 && p[1] == 0 && p[2] == 1) || (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)))
                ls = 0;                               // the source already delivers Annex B
            size_t before = m_Pending.size();
            AppendAnnexB(m_Pending, p, n, ls);
            CaptureParameterSets(m_Pending.data() + before, m_Pending.size() - before, m_Sps, m_Pps);
            buf->Unlock();
            LONGLONG t = 0;
            if (SUCCEEDED(sample->GetSampleTime(&t)))
            {
                m_Pts.push_back(t);
                std::push_heap(m_Pts.begin(), m_Pts.end(), std::greater<LONGLONG>());
            }
            LONGLONG d = 0;
            if (SUCCEEDED(sample->GetSampleDuration(&d)) && d > 0)
                m_Duration = d;
            m_HavePending = !m_Pending.empty();
            m_Draining = false;
        }
        if (buf) buf->Release();
        LeaveCriticalSection(&m_Lock);
        return hr;
    }

    STDMETHODIMP ProcessOutput(DWORD flags, DWORD count, MFT_OUTPUT_DATA_BUFFER *out, DWORD *status) override
    {
        if (flags || count != 1 || !out || !status) return E_INVALIDARG;
        *status = 0;
        out[0].dwStatus = 0;
        out[0].pSample = nullptr;
        out[0].pEvents = nullptr;
        EnterCriticalSection(&m_Lock);
        HRESULT hr = DecodeLoop(&out[0]);
        LeaveCriticalSection(&m_Lock);
        return hr;
    }

private:
    bool ProbeDevice()
    {
        if (m_Dev != INVALID_HANDLE_VALUE)
            return true;
        WCHAR list[1024];
        if (CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_EXYNOS9820_MFC, nullptr, list, ARRAYSIZE(list),
                                          CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || list[0] == 0)
            return false;
        m_Dev = CreateFileW(list, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        return m_Dev != INVALID_HANDLE_VALUE;
    }

    bool OpenSession()
    {
        MFC_OPEN_IN in = { MFC_IOCTL_VERSION, MFC_CODEC_H264 };
        DWORD       got;
        if (m_SessionOpen)
            return true;
        if (!ProbeDevice() || !DeviceIoControl(m_Dev, IOCTL_MFC_OPEN, &in, sizeof(in), nullptr, 0, &got, nullptr))
        {
            Trace("Exynos9820MfcMft: IOCTL_MFC_OPEN failed %lu\n", GetLastError());
            return false;
        }
        m_SessionOpen = true;
        return true;
    }

    void CloseSession()
    {
        DWORD got;
        if (m_SessionOpen)
            DeviceIoControl(m_Dev, IOCTL_MFC_CLOSE, nullptr, 0, nullptr, 0, &got, nullptr);
        m_SessionOpen = false;
        m_Coded = false;
    }

    void ReadSequenceHeader()
    {
        UINT32 n = 0;
        if (FAILED(m_In->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &n)) || n == 0)
            return;
        std::vector<BYTE> blob(n), annexb;
        if (FAILED(m_In->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, blob.data(), n, nullptr)))
            return;
        m_LengthSize = AvcCToAnnexB(blob.data(), n, annexb);
        if (m_LengthSize == 0)
            annexb = blob;                            // already Annex B
        CaptureParameterSets(annexb.data(), annexb.size(), m_Sps, m_Pps);
    }

    HRESULT MakeOutputType(IMFMediaType **type)
    {
        HRESULT hr = MFCreateMediaType(type);
        UINT32  num = 30, den = 1;
        if (m_In)
            MFGetAttributeRatio(m_In, MF_MT_FRAME_RATE, &num, &den);
        if (SUCCEEDED(hr)) hr = (*type)->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = (*type)->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (SUCCEEDED(hr) && m_DispW) hr = MFSetAttributeSize(*type, MF_MT_FRAME_SIZE, m_DispW, m_DispH);
        if (SUCCEEDED(hr) && m_DispW) hr = (*type)->SetUINT32(MF_MT_DEFAULT_STRIDE, m_DispW);
        if (SUCCEEDED(hr) && m_DispW) hr = (*type)->SetUINT32(MF_MT_SAMPLE_SIZE, m_DispW * m_DispH * 3 / 2);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(*type, MF_MT_FRAME_RATE, num, den);
        if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(*type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(hr)) hr = (*type)->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(hr)) hr = (*type)->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        if (SUCCEEDED(hr)) hr = (*type)->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        return hr;
    }

    /** Copy the cropped picture of the IOCTL result into a new sample. **/
    HRESULT MakeSample(const MFC_DECODE_OUT *o, IMFSample **sample)
    {
        const BYTE     *y = reinterpret_cast<const BYTE *>(o + 1);
        const BYTE     *uv = y + o->Width * o->Height;
        IMFMediaBuffer *buf = nullptr;
        BYTE           *dst = nullptr;
        DWORD           size = m_DispW * m_DispH * 3 / 2;
        HRESULT         hr = MFCreateMemoryBuffer(size, &buf);
        if (SUCCEEDED(hr)) hr = buf->Lock(&dst, nullptr, nullptr);
        if (SUCCEEDED(hr))
        {
            for (UINT32 r = 0; r < m_DispH; r++)
                memcpy(dst + r * m_DispW, y + (o->CropTop + r) * o->Width + o->CropLeft, m_DispW);
            dst += m_DispW * m_DispH;
            for (UINT32 r = 0; r < m_DispH / 2; r++)
                memcpy(dst + r * m_DispW, uv + (o->CropTop / 2 + r) * o->Width + (o->CropLeft & ~1u), m_DispW);
            buf->Unlock();
            hr = buf->SetCurrentLength(size);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(sample);
        if (SUCCEEDED(hr)) hr = (*sample)->AddBuffer(buf);
        if (SUCCEEDED(hr))
        {
            LONGLONG t = m_LastPts + m_Duration;
            if (!m_Pts.empty())
            {
                std::pop_heap(m_Pts.begin(), m_Pts.end(), std::greater<LONGLONG>());
                t = m_Pts.back();
                m_Pts.pop_back();
            }
            m_LastPts = t;
            (*sample)->SetSampleTime(t);
            if (m_Duration > 0)
                (*sample)->SetSampleDuration(m_Duration);
            (*sample)->SetUINT32(MFSampleExtension_CleanPoint, o->FrameType == 1);
        }
        if (buf) buf->Release();
        return hr;
    }

    HRESULT DecodeLoop(MFT_OUTPUT_DATA_BUFFER *out)
    {
        if (!m_In || !m_Out)
            return MF_E_TRANSFORM_TYPE_NOT_SET;
        if (m_FormatChange)
        {
            out->dwStatus = MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
            return MF_E_TRANSFORM_STREAM_CHANGE;
        }
        for (int guard = 0; guard < 64; guard++)
        {
            if (!m_HavePending && !(m_Draining && m_SessionOpen))
            {
                if (m_Draining)
                    m_Draining = false;              // drain complete
                return MF_E_TRANSFORM_NEED_MORE_INPUT;
            }
            if (!OpenSession())
                return E_FAIL;
            DWORD window = m_HavePending ? (DWORD)(m_Pending.size() - m_PendingOff) : 0;
            if (window > MFC_MAX_WINDOW)
                window = MFC_MAX_WINDOW;
            size_t need = sizeof(MFC_DECODE_OUT) + (m_Coded ? (size_t)m_CodedW * m_CodedH * 3 / 2 : 0);
            if (m_OutBuf.size() < need)
                m_OutBuf.resize(need);
            DWORD got = 0;
            if (!DeviceIoControl(m_Dev, IOCTL_MFC_DECODE, window ? m_Pending.data() + m_PendingOff : nullptr, window,
                                 m_OutBuf.data(), (DWORD)m_OutBuf.size(), &got, nullptr))
            {
                Trace("Exynos9820MfcMft: IOCTL_MFC_DECODE failed %lu\n", GetLastError());
                m_Pending.clear();
                m_HavePending = false;
                CloseSession();
                return E_FAIL;
            }
            const MFC_DECODE_OUT *o = reinterpret_cast<const MFC_DECODE_OUT *>(m_OutBuf.data());
            if (!(o->Flags & MFC_DEC_NOT_CONSUMED))
            {
                m_PendingOff += o->Consumed;
                if (window == 0 || m_PendingOff + 4 >= m_Pending.size() ||
                    ((o->Flags & MFC_DEC_ERROR) && o->Consumed == 0))
                {
                    m_Pending.clear();
                    m_PendingOff = 0;
                    m_HavePending = false;
                }
            }
            if (o->Flags & MFC_DEC_HEADER)
            {
                UINT32 w = o->Width - o->CropLeft - o->CropRight, h = o->Height - o->CropTop - o->CropBottom;
                m_Coded = true;
                m_CodedW = o->Width;
                m_CodedH = o->Height;
                m_HeaderSeen = true;
                if (w != m_DispW || h != m_DispH)
                {
                    m_DispW = w;
                    m_DispH = h;
                    m_FormatChange = true;
                    out->dwStatus = MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE;
                    return MF_E_TRANSFORM_STREAM_CHANGE;
                }
                continue;
            }
            if (o->Flags & MFC_DEC_FRAME)
                return MakeSample(o, &out->pSample);
            if (o->Flags & MFC_DEC_EOS)
            {
                CloseSession();                       // drained: the next stream opens a fresh session
                m_Draining = false;
                return MF_E_TRANSFORM_NEED_MORE_INPUT;
            }
        }
        return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }

    volatile LONG     m_Ref = 1;
    CRITICAL_SECTION  m_Lock;
    IMFMediaType     *m_In = nullptr;
    IMFMediaType     *m_Out = nullptr;
    IMFAttributes    *m_Attr = nullptr;
    HANDLE            m_Dev = INVALID_HANDLE_VALUE;
    bool              m_SessionOpen = false;
    bool              m_Coded = false;
    bool              m_HeaderSeen = false;
    bool              m_FormatChange = false;
    bool              m_HavePending = false;
    bool              m_Draining = false;
    UINT32            m_LengthSize = 0;
    UINT32            m_DispW = 0, m_DispH = 0, m_CodedW = 0, m_CodedH = 0;
    std::vector<BYTE> m_Pending, m_Sps, m_Pps, m_OutBuf;
    size_t            m_PendingOff = 0;
    std::vector<LONGLONG> m_Pts;                  // min-heap of queued input timestamps
    LONGLONG          m_Duration = 333333, m_LastPts = 0;
};

class CFactory : public IClassFactory
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *ppv = static_cast<IClassFactory *>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown *outer, REFIID riid, void **ppv) override
    {
        if (outer) return CLASS_E_NOAGGREGATION;
        CMfcDecoder *d = new (std::nothrow) CMfcDecoder();
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->QueryInterface(riid, ppv);
        d->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override
    {
        lock ? InterlockedIncrement(&g_Locks) : InterlockedDecrement(&g_Locks);
        return S_OK;
    }
};

static CFactory g_Factory;

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    if (clsid != CLSID_Exynos9820MfcH264)
        return CLASS_E_CLASSNOTAVAILABLE;
    return g_Factory.QueryInterface(riid, ppv);
}

STDAPI DllCanUnloadNow() { return (g_Objects == 0 && g_Locks == 0) ? S_OK : S_FALSE; }

static const WCHAR kClsidKey[] = L"Software\\Classes\\CLSID\\{6A2E8C51-0B7D-4C3E-8F14-982AC0DE0D11}";

STDAPI DllRegisterServer()
{
    WCHAR path[MAX_PATH];
    HKEY  key;
    if (!GetModuleFileNameW(g_Module, path, MAX_PATH))
        return HRESULT_FROM_WIN32(GetLastError());
    WCHAR sub[200];
    wsprintfW(sub, L"%s\\InprocServer32", kClsidKey);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return E_ACCESSDENIED;
    RegSetValueExW(key, nullptr, 0, REG_SZ, (const BYTE *)path, (DWORD)((wcslen(path) + 1) * sizeof(WCHAR)));
    RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ, (const BYTE *)L"Both", sizeof(L"Both"));
    RegCloseKey(key);
    MFT_REGISTER_TYPE_INFO in = { MFMediaType_Video, MFVideoFormat_H264 };
    MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, MFVideoFormat_NV12 };
    return MFTRegister(CLSID_Exynos9820MfcH264, MFT_CATEGORY_VIDEO_DECODER,
                       const_cast<LPWSTR>(L"Exynos9820 MFC H.264 Decoder (hardware)"), 0, 1, &in, 1, &out, nullptr);
}

STDAPI DllUnregisterServer()
{
    MFTUnregister(CLSID_Exynos9820MfcH264);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClsidKey);
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_Module = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
