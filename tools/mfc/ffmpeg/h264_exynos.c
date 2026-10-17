/*
 * h264_exynos.c - FFmpeg H.264 decoder wrapper for the Samsung Exynos 9820 MFC v13 under Windows (exynos-woa D10 V5,
 * notes/mpv-build.md). Talks to the Exynos9820Mfc KMDF driver through its IOCTLs (drivers/Exynos9820Mfc/MfcIoctl.h):
 * one IOCTL_MFC_DECODE per access unit (Annex B, via h264_mp4toannexb), pictures come back in display order as packed
 * NV12; their pts is the smallest queued packet pts. Seeking (flush) closes the hardware session; the next packet
 * reopens it with the parameter sets replayed.
 *
 * Built into the project's custom ARM64 FFmpeg only (tools/mfc/ffmpeg/apply.sh). LGPL-2.1-or-later like FFmpeg.
 */
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <winioctl.h>

#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "MfcIoctl.h"

#define PTS_MAX 64

typedef struct ExynosMfcContext {
    const AVClass *class;
    HANDLE   dev;
    int      session;              /* hardware session open */
    int      header;               /* sequence header seen in this session */
    AVPacket *pkt;
    uint8_t  *buf;                 /* pending stream: replayed parameter sets + current access unit */
    size_t   buf_len, buf_cap, buf_off;
    uint8_t  *sps_pps;             /* last SPS/PPS (Annex B), replayed when a session opens */
    size_t   sps_pps_len;
    uint8_t  *out;
    size_t   out_cap;
    unsigned coded_w, coded_h;
    int64_t  pts[PTS_MAX];
    int      npts;
    int      draining, eos;
} ExynosMfcContext;

static void pts_push(ExynosMfcContext *s, int64_t v)
{
    if (s->npts < PTS_MAX)
        s->pts[s->npts++] = v;
}

static int64_t pts_pop_min(ExynosMfcContext *s)
{
    int i, m = 0;
    int64_t v;

    if (!s->npts)
        return AV_NOPTS_VALUE;
    for (i = 1; i < s->npts; i++)
        if (s->pts[i] < s->pts[m])
            m = i;
    v = s->pts[m];
    s->pts[m] = s->pts[--s->npts];
    return v;
}

static int buf_append(ExynosMfcContext *s, const uint8_t *p, size_t n)
{
    if (s->buf_len + n > s->buf_cap) {
        size_t cap = FFMAX(s->buf_cap * 2, s->buf_len + n + 4096);
        uint8_t *b = av_realloc(s->buf, cap);
        if (!b)
            return AVERROR(ENOMEM);
        s->buf = b;
        s->buf_cap = cap;
    }
    memcpy(s->buf + s->buf_len, p, n);
    s->buf_len += n;
    return 0;
}

/* keep the latest SPS (7) + PPS (8) NAL units of an Annex-B buffer */
static void capture_sps_pps(ExynosMfcContext *s, const uint8_t *p, size_t n)
{
    size_t i = 0, sps_s = 0, sps_e = 0, pps_s = 0, pps_e = 0;

    while (i + 3 < n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            size_t st = i + 3, en = st;
            while (en + 2 < n && !(p[en] == 0 && p[en + 1] == 0 && (p[en + 2] == 1 || p[en + 2] == 0)))
                en++;
            if (en + 2 >= n)
                en = n;
            if ((p[st] & 0x1F) == 7) { sps_s = st; sps_e = en; }
            if ((p[st] & 0x1F) == 8) { pps_s = st; pps_e = en; }
            i = en;
        } else
            i++;
    }
    if (sps_e && pps_e) {
        static const uint8_t sc[4] = { 0, 0, 0, 1 };
        size_t len = 8 + (sps_e - sps_s) + (pps_e - pps_s);
        uint8_t *b = av_realloc(s->sps_pps, len);
        if (!b)
            return;
        memcpy(b, sc, 4);
        memcpy(b + 4, p + sps_s, sps_e - sps_s);
        memcpy(b + 4 + (sps_e - sps_s), sc, 4);
        memcpy(b + 8 + (sps_e - sps_s), p + pps_s, pps_e - pps_s);
        s->sps_pps = b;
        s->sps_pps_len = len;
    }
}

static int open_device(AVCodecContext *avctx, ExynosMfcContext *s)
{
    WCHAR list[1024];

    if (CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_EXYNOS9820_MFC, NULL, list, FF_ARRAY_ELEMS(list),
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || !list[0]) {
        av_log(avctx, AV_LOG_ERROR, "no Exynos9820Mfc device (driver not running)\n");
        return AVERROR(ENODEV);
    }
    s->dev = CreateFileW(list, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);   // IOCTLs are FILE_ANY_ACCESS; the device grants users read only (INF 1.0.0.9)
    if (s->dev == INVALID_HANDLE_VALUE) {
        av_log(avctx, AV_LOG_ERROR, "cannot open the Exynos9820Mfc device (%lu) - in use by another player?\n",
               GetLastError());
        return AVERROR(EBUSY);
    }
    return 0;
}

static int session_open(AVCodecContext *avctx, ExynosMfcContext *s)
{
    MFC_OPEN_IN in = { MFC_IOCTL_VERSION, MFC_CODEC_H264 };
    DWORD got;

    if (s->session)
        return 0;
    if (!DeviceIoControl(s->dev, IOCTL_MFC_OPEN, &in, sizeof(in), NULL, 0, &got, NULL)) {
        av_log(avctx, AV_LOG_ERROR, "IOCTL_MFC_OPEN failed (%lu)\n", GetLastError());
        return AVERROR(EIO);
    }
    s->session = 1;
    s->header = 0;
    return 0;
}

static void session_close(ExynosMfcContext *s)
{
    DWORD got;

    if (s->session)
        DeviceIoControl(s->dev, IOCTL_MFC_CLOSE, NULL, 0, NULL, 0, &got, NULL);
    s->session = 0;
    s->header = 0;
}

static av_cold int exynos_init(AVCodecContext *avctx)
{
    ExynosMfcContext *s = avctx->priv_data;

    s->dev = INVALID_HANDLE_VALUE;
    avctx->pix_fmt = AV_PIX_FMT_NV12;
    s->pkt = av_packet_alloc();
    if (!s->pkt)
        return AVERROR(ENOMEM);
    if (avctx->extradata_size > 4 && avctx->extradata[0] == 0)   /* Annex-B extradata (raw .h264 / after the bsf) */
        capture_sps_pps(s, avctx->extradata, avctx->extradata_size);
    return open_device(avctx, s);
}

static av_cold int exynos_close(AVCodecContext *avctx)
{
    ExynosMfcContext *s = avctx->priv_data;

    session_close(s);
    if (s->dev != INVALID_HANDLE_VALUE)
        CloseHandle(s->dev);
    s->dev = INVALID_HANDLE_VALUE;
    av_packet_free(&s->pkt);
    av_freep(&s->buf);
    av_freep(&s->sps_pps);
    av_freep(&s->out);
    return 0;
}

static void exynos_flush(AVCodecContext *avctx)
{
    ExynosMfcContext *s = avctx->priv_data;

    session_close(s);
    s->buf_len = s->buf_off = 0;
    s->npts = 0;
    s->draining = s->eos = 0;
    av_packet_unref(s->pkt);
}

static int output_frame(AVCodecContext *avctx, ExynosMfcContext *s, const MFC_DECODE_OUT *o, AVFrame *frame)
{
    const uint8_t *y  = (const uint8_t *)(o + 1);
    const uint8_t *uv = y + o->Width * o->Height;
    unsigned w = o->Width - o->CropLeft - o->CropRight, h = o->Height - o->CropTop - o->CropBottom, r;
    int ret;

    if ((ret = ff_set_dimensions(avctx, w, h)) < 0)
        return ret;
    avctx->coded_width  = o->Width;
    avctx->coded_height = o->Height;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    for (r = 0; r < h; r++)
        memcpy(frame->data[0] + r * frame->linesize[0], y + (o->CropTop + r) * o->Width + o->CropLeft, w);
    for (r = 0; r < h / 2; r++)
        memcpy(frame->data[1] + r * frame->linesize[1], uv + (o->CropTop / 2 + r) * o->Width + (o->CropLeft & ~1u), w);
    frame->pts       = pts_pop_min(s);
    frame->pkt_dts   = AV_NOPTS_VALUE;
    frame->pict_type = o->FrameType == 1 ? AV_PICTURE_TYPE_I : o->FrameType == 2 ? AV_PICTURE_TYPE_P :
                       o->FrameType == 3 ? AV_PICTURE_TYPE_B : AV_PICTURE_TYPE_NONE;
    if (o->FrameType == 1)
        frame->flags |= AV_FRAME_FLAG_KEY;
    if (o->Flags & MFC_DEC_WARNING)
        frame->decode_error_flags |= FF_DECODE_ERROR_CONCEALMENT_ACTIVE;
    return 0;
}

static int exynos_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    ExynosMfcContext *s = avctx->priv_data;
    int guard, ret;

    for (guard = 0; guard < 64; guard++) {
        const MFC_DECODE_OUT *o;
        DWORD window, got;
        size_t need;

        if (s->eos)
            return AVERROR_EOF;
        if (s->buf_off >= s->buf_len && !s->draining) {
            ret = ff_decode_get_packet(avctx, s->pkt);
            if (ret == AVERROR_EOF) {
                s->draining = 1;
                if (!s->session)
                    return AVERROR_EOF;
            } else if (ret < 0)
                return ret;
            else {
                s->buf_len = s->buf_off = 0;
                if (!s->session && s->sps_pps_len)          /* a new session needs the parameter sets first */
                    if ((ret = buf_append(s, s->sps_pps, s->sps_pps_len)) < 0)
                        return ret;
                if ((ret = buf_append(s, s->pkt->data, s->pkt->size)) < 0)
                    return ret;
                capture_sps_pps(s, s->pkt->data, s->pkt->size);
                pts_push(s, s->pkt->pts != AV_NOPTS_VALUE ? s->pkt->pts : s->pkt->dts);
                av_packet_unref(s->pkt);
            }
        }
        if ((ret = session_open(avctx, s)) < 0)
            return ret;

        window = s->draining && s->buf_off >= s->buf_len ? 0 : (DWORD)(s->buf_len - s->buf_off);
        if (window > MFC_MAX_WINDOW)
            window = MFC_MAX_WINDOW;
        need = sizeof(MFC_DECODE_OUT) + (s->header ? (size_t)s->coded_w * s->coded_h * 3 / 2 : 0);
        if (need > s->out_cap) {
            av_freep(&s->out);
            s->out = av_malloc(need);
            if (!s->out)
                return AVERROR(ENOMEM);
            s->out_cap = need;
        }
        if (!DeviceIoControl(s->dev, IOCTL_MFC_DECODE, window ? s->buf + s->buf_off : NULL, window, s->out,
                             (DWORD)s->out_cap, &got, NULL)) {
            av_log(avctx, AV_LOG_ERROR, "IOCTL_MFC_DECODE failed (%lu)\n", GetLastError());
            session_close(s);
            s->buf_len = s->buf_off = 0;
            return AVERROR(EIO);
        }
        o = (const MFC_DECODE_OUT *)s->out;
        if (!(o->Flags & MFC_DEC_NOT_CONSUMED)) {
            s->buf_off += o->Consumed;
            if (window == 0 || s->buf_off + 4 >= s->buf_len || ((o->Flags & MFC_DEC_ERROR) && !o->Consumed))
                s->buf_off = s->buf_len;
        }
        if (o->Flags & MFC_DEC_ERROR)
            av_log(avctx, AV_LOG_WARNING, "MFC error 0x%x (R2H %u, status 0x%x)\n", o->Error, o->R2h,
                   o->DisplayStatus);
        if (o->Flags & MFC_DEC_HEADER) {
            s->header  = 1;
            s->coded_w = o->Width;
            s->coded_h = o->Height;
            if ((ret = ff_set_dimensions(avctx, o->Width - o->CropLeft - o->CropRight,
                                         o->Height - o->CropTop - o->CropBottom)) < 0)
                return ret;
            continue;
        }
        if (o->Flags & MFC_DEC_FRAME)
            return output_frame(avctx, s, o, frame);
        if (o->Flags & MFC_DEC_EOS) {
            s->eos = 1;
            session_close(s);
            return AVERROR_EOF;
        }
    }
    return AVERROR(EAGAIN);
}

static const AVClass exynos_class = {
    .class_name = "h264_exynos",
    .item_name  = av_default_item_name,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_h264_exynos_decoder = {
    .p.name         = "h264_exynos",
    CODEC_LONG_NAME("Exynos 9820 MFC H.264 hardware decoder"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(ExynosMfcContext),
    .p.priv_class   = &exynos_class,
    .init           = exynos_init,
    FF_CODEC_RECEIVE_FRAME_CB(exynos_receive_frame),
    .flush          = exynos_flush,
    .close          = exynos_close,
    .bsfs           = "h264_mp4toannexb",
    .p.capabilities = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE | FF_CODEC_CAP_INIT_CLEANUP,
    .p.wrapper_name = "exynosmfc",
};
