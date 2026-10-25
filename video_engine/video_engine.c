/*
 * Motor Profissional de Compartilhamento de Tela / Janela (60 FPS, Ultrawide, H.264 MFT + Fallback)
 * - Captura: DXGI Desktop Duplication (Monitores) ou HWND PrintWindow/BitBlt (Janelas de Aplicativos)
 * - Preservação de Proporção (Ultrawide 21:9, 32:9, 16:9, 16:10, janelas arbitrárias) dentro do limite 720p/1080p
 * - Codificação: Windows Media Foundation H.264 MFT (Low Latency) com fallback automático para RLE rápido
 * - Transporte: UDP fragmentado cifrado com AES-256-GCM (Windows CNG)
 * - Multi-slot Frame Assembly (4 slots) para evitar descarte por pacotes sobrepostos
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../audio_engine/c_crypto.h"

#define EXPORT __declspec(dllexport)

#define MAX_W            2560
#define MAX_H            1080
#define MAX_PIXELS       (MAX_W * MAX_H)
#define MAX_RAW_BYTES    (MAX_PIXELS * 4)
#define MAX_COMP_BYTES   (MAX_RAW_BYTES + 65536)
#define CHUNK_PAYLOAD    1180
#define MAX_CHUNKS       ((MAX_COMP_BYTES / CHUNK_PAYLOAD) + 8)
#define MAX_TARGETS      8
#define MAX_STREAMS      2
#define NUM_FRAME_SLOTS  4
#define FRAME_TIMEOUT_MS 600

#define CODEC_RLE        0x01
#define CODEC_H264       0x02

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

#pragma pack(push, 1)
typedef struct {
    uint32_t frame_id;
    uint16_t chunk_index;
    uint16_t total_chunks;
    uint16_t width;
    uint16_t height;
    uint8_t  codec;
} PacketHeader;
#pragma pack(pop)

typedef void (*FrameCallback)(int stream_id, const unsigned char *data, int width, int height, int stride);

typedef struct {
    IMFTransform *mft;
    int           width;
    int           height;
    int           fps;
    int           bitrate;
    int           ready;
} MfCodec;

typedef struct {
    uint32_t frame_id;
    uint16_t total_chunks;
    uint16_t received_chunks;
    uint16_t width;
    uint16_t height;
    uint8_t  codec;
    DWORD    start_ts;
    uint32_t comp_size;
    uint8_t  chunk_mask[MAX_CHUNKS];
    uint8_t *comp_buf;
} FrameSlot;

typedef struct {
    uint32_t   sender_id;
    DWORD      last_rx;
    uint32_t   last_rendered_frame_id;
    FrameSlot  slots[NUM_FRAME_SLOTS];
    uint8_t   *bgra_buf;
    uint8_t   *nv12_buf;
    MfCodec    h264_dec;
} RxStream;

static CRITICAL_SECTION g_lock;
static CryptoContext    g_crypto;
static volatile LONG    g_running   = 0;
static volatile LONG    g_capturing = 0;

/* Parâmetros da captura selecionados no ScreenShareDialog */
static volatile LONG    g_cfg_max_h   = 720;
static volatile LONG    g_cfg_fps     = 60;
static volatile LONG    g_cfg_bitrate = 4000000;
static volatile LONG    g_cfg_monitor = 0;
static volatile HWND    g_cfg_hwnd    = NULL;

static SOCKET             g_sock = INVALID_SOCKET;
static struct sockaddr_in g_targets[MAX_TARGETS];
static int                g_num_targets = 0;

static HANDLE        g_rx_thread  = NULL;
static HANDLE        g_cap_thread = NULL;
static FrameCallback g_callback   = NULL;
static RxStream      g_rx_streams[MAX_STREAMS];

static uint8_t *g_cap_bgra  = NULL;
static uint8_t *g_cap_nv12  = NULL;
static uint8_t *g_cap_comp  = NULL;
static uint8_t *g_prev_bgra = NULL;

static int is_known_target_ip(uint32_t ip_addr) {
    int ok = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_num_targets; i++) {
        if (g_targets[i].sin_addr.s_addr == ip_addr) {
            ok = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

/* Calcula dimensões de saída preservando proporção exata (inclusive Ultrawide 21:9 e 32:9) */
static void compute_scaled_dims(int src_w, int src_h, int target_max_h, int *out_w, int *out_h) {
    if (src_w <= 0 || src_h <= 0) {
        *out_w = 1280;
        *out_h = 720;
        return;
    }
    int max_h = (target_max_h >= 1080) ? 1080 : 720;
    /* Permite largura até 2560 em 1080p (Ultrawide 2560x1080) ou 1720 em 720p (Ultrawide 1720x720) */
    int max_w = (max_h == 1080) ? 2560 : 1720;

    double scale_h = (src_h > max_h) ? ((double)max_h / (double)src_h) : 1.0;
    double scale_w = (src_w > max_w) ? ((double)max_w / (double)src_w) : 1.0;
    double scale = (scale_h < scale_w) ? scale_h : scale_w;

    int w = ((int)(src_w * scale + 0.5)) & ~1; /* par para compatibilidade NV12/H.264 */
    int h = ((int)(src_h * scale + 0.5)) & ~1;
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    if (w > MAX_W) w = MAX_W;
    if (h > MAX_H) h = MAX_H;
    *out_w = w;
    *out_h = h;
}

/* Conversão rápida BGRA -> NV12 (BT.601) para o encoder H.264 MFT */
static void bgra_to_nv12(const uint8_t *bgra, uint8_t *nv12, int w, int h) {
    uint8_t *y_plane  = nv12;
    uint8_t *uv_plane = nv12 + (w * h);

    for (int y = 0; y < h; y += 2) {
        const uint8_t *row0 = bgra + (y * w * 4);
        const uint8_t *row1 = bgra + ((y + 1) * w * 4);
        uint8_t *y_out0 = y_plane + (y * w);
        uint8_t *y_out1 = y_plane + ((y + 1) * w);
        uint8_t *uv_out = uv_plane + ((y / 2) * w);

        for (int x = 0; x < w; x += 2) {
            int b00 = row0[0], g00 = row0[1], r00 = row0[2];
            int b01 = row0[4], g01 = row0[5], r01 = row0[6];
            int b10 = row1[0], g10 = row1[1], r10 = row1[2];
            int b11 = row1[4], g11 = row1[5], r11 = row1[6];

            y_out0[x]     = (uint8_t)(((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16);
            y_out0[x + 1] = (uint8_t)(((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16);
            y_out1[x]     = (uint8_t)(((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16);
            y_out1[x + 1] = (uint8_t)(((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16);

            int u = ((-38 * r00 - 74 * g00 + 112 * b00 + 128) >> 8) + 128;
            int v = ((112 * r00 - 94 * g00 -  18 * b00 + 128) >> 8) + 128;
            uv_out[x]     = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            uv_out[x + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));

            row0 += 8;
            row1 += 8;
        }
    }
}

/* Conversão rápida NV12 -> BGRA após decodificação H.264 MFT */
static void nv12_to_bgra(const uint8_t *nv12, uint8_t *bgra, int w, int h) {
    const uint8_t *y_plane  = nv12;
    const uint8_t *uv_plane = nv12 + (w * h);

    for (int y = 0; y < h; y++) {
        const uint8_t *y_row  = y_plane + (y * w);
        const uint8_t *uv_row = uv_plane + ((y >> 1) * w);
        uint32_t *dst_row = (uint32_t *)(bgra + (y * w * 4));

        for (int x = 0; x < w; x++) {
            int c = (int)y_row[x] - 16;
            int d = (int)uv_row[x & ~1] - 128;
            int e = (int)uv_row[(x & ~1) + 1] - 128;
            if (c < 0) c = 0;

            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;

            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            dst_row[x] = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xFF000000u;
        }
    }
}

/* Fallback RLE rápido */
static uint32_t compress_rle(const uint8_t *bgra, int w, int h, int bitrate, uint8_t *out) {
    int total_pixels = w * h;
    int shift = (bitrate >= 6000000) ? 2 : 3;
    uint8_t mask = (uint8_t)(0xFF << shift);

    const uint32_t *px = (const uint32_t *)bgra;
    uint8_t *dst = out;
    int i = 0;

    while (i < total_pixels) {
        uint32_t p0 = px[i];
        uint8_t b0 = ((uint8_t)(p0      )) & mask;
        uint8_t g0 = ((uint8_t)(p0 >>  8)) & mask;
        uint8_t r0 = ((uint8_t)(p0 >> 16)) & mask;
        uint32_t q0 = (uint32_t)b0 | ((uint32_t)g0 << 8) | ((uint32_t)r0 << 16);

        int run = 1;
        while (i + run < total_pixels && run < 127) {
            uint32_t p1 = px[i + run];
            uint32_t q1 = ((p1 & 0xFF) & mask) |
                          ((((p1 >> 8) & 0xFF) & mask) << 8) |
                          ((((p1 >> 16) & 0xFF) & mask) << 16);
            if (q1 != q0) break;
            run++;
        }

        if (run >= 2) {
            *dst++ = (uint8_t)(0x80 | run);
            *dst++ = b0;
            *dst++ = g0;
            *dst++ = r0;
            i += run;
        } else {
            uint8_t *hdr = dst++;
            int lit = 0;
            while (i < total_pixels && lit < 127) {
                uint32_t p = px[i];
                uint8_t b = ((uint8_t)(p      )) & mask;
                uint8_t g = ((uint8_t)(p >>  8)) & mask;
                uint8_t r = ((uint8_t)(p >> 16)) & mask;
                if (i + 1 < total_pixels) {
                    uint32_t pn = px[i + 1];
                    if ((((pn & 0xFF) & mask) == b) &&
                        ((((pn >> 8) & 0xFF) & mask) == g) &&
                        ((((pn >> 16) & 0xFF) & mask) == r)) {
                        break;
                    }
                }
                *dst++ = b;
                *dst++ = g;
                *dst++ = r;
                i++;
                lit++;
            }
            *hdr = (uint8_t)lit;
        }
    }
    return (uint32_t)(dst - out);
}

static int decompress_rle(const uint8_t *in, uint32_t in_len, uint8_t *bgra, int w, int h) {
    int total_pixels = w * h;
    uint32_t *dst = (uint32_t *)bgra;
    const uint8_t *src = in;
    const uint8_t *end = in + in_len;
    int p = 0;

    while (src < end && p < total_pixels) {
        uint8_t tag = *src++;
        if (tag & 0x80) {
            int run = tag & 0x7F;
            if (src + 3 > end) return -1;
            uint32_t val = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | 0xFF000000u;
            src += 3;
            int lim = (p + run <= total_pixels) ? (p + run) : total_pixels;
            while (p < lim) dst[p++] = val;
        } else {
            int lit = tag;
            if (src + lit * 3 > end) return -1;
            for (int k = 0; k < lit && p < total_pixels; k++) {
                dst[p++] = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | 0xFF000000u;
                src += 3;
            }
        }
    }
    return (p == total_pixels) ? 0 : -1;
}

/* Inicializa Encoder ou Decoder H.264 via Windows Media Foundation (MFT) */
static void mf_close_codec(MfCodec *c) {
    if (c->mft) {
        IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        IMFTransform_Release(c->mft);
        c->mft = NULL;
    }
    c->ready = 0;
}

static int mf_init_h264_encoder(MfCodec *c, int w, int h, int fps, int bitrate) {
    mf_close_codec(c);
    MFT_REGISTER_TYPE_INFO in_info  = { MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO out_info = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate **activates = NULL;
    UINT32 count = 0;

    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                           &in_info, &out_info, &activates, &count);
    if (FAILED(hr) || count == 0) {
        return -1;
    }

    hr = IMFActivate_ActivateObject(activates[0], &IID_IMFTransform, (void **)&c->mft);
    for (UINT32 i = 0; i < count; i++) {
        IMFActivate_Release(activates[i]);
    }
    CoTaskMemFree(activates);
    if (FAILED(hr) || !c->mft) return -2;

    IMFMediaType *out_type = NULL;
    MFCreateMediaType(&out_type);
    IMFMediaType_SetGUID(out_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(out_type, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    IMFMediaType_SetUINT32(out_type, &MF_MT_AVG_BITRATE, (UINT32)bitrate);
    IMFMediaType_SetUINT32(out_type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT64(out_type, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    IMFMediaType_SetUINT64(out_type, &MF_MT_FRAME_RATE, ((UINT64)fps << 32) | 1);
    hr = IMFTransform_SetOutputType(c->mft, 0, out_type, 0);
    IMFMediaType_Release(out_type);
    if (FAILED(hr)) { mf_close_codec(c); return -3; }

    IMFMediaType *in_type = NULL;
    MFCreateMediaType(&in_type);
    IMFMediaType_SetGUID(in_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(in_type, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    IMFMediaType_SetUINT32(in_type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT64(in_type, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    IMFMediaType_SetUINT64(in_type, &MF_MT_FRAME_RATE, ((UINT64)fps << 32) | 1);
    hr = IMFTransform_SetInputType(c->mft, 0, in_type, 0);
    IMFMediaType_Release(in_type);
    if (FAILED(hr)) { mf_close_codec(c); return -4; }

    IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    c->width = w; c->height = h; c->fps = fps; c->bitrate = bitrate;
    c->ready = 1;
    return 0;
}

static int mf_init_h264_decoder(MfCodec *c, int w, int h) {
    mf_close_codec(c);
    MFT_REGISTER_TYPE_INFO in_info  = { MFMediaType_Video, MFVideoFormat_H264 };
    MFT_REGISTER_TYPE_INFO out_info = { MFMediaType_Video, MFVideoFormat_NV12 };

    IMFActivate **activates = NULL;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           &in_info, &out_info, &activates, &count);
    if (FAILED(hr) || count == 0) return -1;

    hr = IMFActivate_ActivateObject(activates[0], &IID_IMFTransform, (void **)&c->mft);
    for (UINT32 i = 0; i < count; i++) IMFActivate_Release(activates[i]);
    CoTaskMemFree(activates);
    if (FAILED(hr) || !c->mft) return -2;

    IMFMediaType *in_type = NULL;
    MFCreateMediaType(&in_type);
    IMFMediaType_SetGUID(in_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(in_type, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    IMFMediaType_SetUINT32(in_type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT64(in_type, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    hr = IMFTransform_SetInputType(c->mft, 0, in_type, 0);
    IMFMediaType_Release(in_type);
    if (FAILED(hr)) { mf_close_codec(c); return -3; }

    IMFMediaType *out_type = NULL;
    MFCreateMediaType(&out_type);
    IMFMediaType_SetGUID(out_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(out_type, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
    IMFMediaType_SetUINT32(out_type, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFMediaType_SetUINT64(out_type, &MF_MT_FRAME_SIZE, ((UINT64)w << 32) | (UINT32)h);
    hr = IMFTransform_SetOutputType(c->mft, 0, out_type, 0);
    IMFMediaType_Release(out_type);
    if (FAILED(hr)) { mf_close_codec(c); return -4; }

    IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(c->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    c->width = w; c->height = h;
    c->ready = 1;
    return 0;
}

static uint32_t mf_process_sample(MfCodec *c, const uint8_t *in_bytes, uint32_t in_len,
                                  LONGLONG ts_100ns, LONGLONG dur_100ns,
                                  uint8_t *out_bytes, uint32_t max_out) {
    if (!c->ready || !c->mft) return 0;

    IMFMediaBuffer *in_buf = NULL;
    if (FAILED(MFCreateMemoryBuffer(in_len, &in_buf))) return 0;
    BYTE *ptr = NULL;
    if (SUCCEEDED(IMFMediaBuffer_Lock(in_buf, &ptr, NULL, NULL))) {
        memcpy(ptr, in_bytes, in_len);
        IMFMediaBuffer_Unlock(in_buf);
        IMFMediaBuffer_SetCurrentLength(in_buf, in_len);
    }

    IMFSample *in_sample = NULL;
    MFCreateSample(&in_sample);
    IMFSample_AddBuffer(in_sample, in_buf);
    IMFSample_SetSampleTime(in_sample, ts_100ns);
    IMFSample_SetSampleDuration(in_sample, dur_100ns);
    IMFMediaBuffer_Release(in_buf);

    HRESULT hr = IMFTransform_ProcessInput(c->mft, 0, in_sample, 0);
    IMFSample_Release(in_sample);
    if (FAILED(hr)) return 0;

    IMFMediaBuffer *out_buf = NULL;
    MFCreateMemoryBuffer(max_out, &out_buf);
    IMFSample *out_sample = NULL;
    MFCreateSample(&out_sample);
    IMFSample_AddBuffer(out_sample, out_buf);

    MFT_OUTPUT_DATA_BUFFER out_data;
    memset(&out_data, 0, sizeof(out_data));
    out_data.pSample = out_sample;
    DWORD status = 0;

    hr = IMFTransform_ProcessOutput(c->mft, 0, 1, &out_data, &status);
    if (out_data.pEvents) IUnknown_Release((IUnknown *)out_data.pEvents);

    uint32_t written = 0;
    if (SUCCEEDED(hr)) {
        IMFMediaBuffer *res_buf = NULL;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(out_data.pSample, &res_buf))) {
            BYTE *optr = NULL;
            DWORD olen = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(res_buf, &optr, NULL, &olen))) {
                if (olen <= max_out) {
                    memcpy(out_bytes, optr, olen);
                    written = olen;
                }
                IMFMediaBuffer_Unlock(res_buf);
            }
            IMFMediaBuffer_Release(res_buf);
        }
    }

    IMFSample_Release(out_sample);
    IMFMediaBuffer_Release(out_buf);
    return written;
}

static void scale_bgra(const uint8_t *src, int src_w, int src_h, int src_pitch, uint8_t *dst, int dst_w, int dst_h) {
    for (int y = 0; y < dst_h; y++) {
        int sy = (y * src_h) / dst_h;
        const uint32_t *src_row = (const uint32_t *)(src + sy * src_pitch);
        uint32_t *dst_row = (uint32_t *)(dst + y * dst_w * 4);
        for (int x = 0; x < dst_w; x++) {
            int sx = (x * src_w) / dst_w;
            dst_row[x] = src_row[sx] | 0xFF000000u;
        }
    }
}

static void send_encoded_frame(uint32_t frame_id, const uint8_t *comp, uint32_t comp_len,
                               int w, int h, uint8_t codec) {
    if (!g_crypto.ready) return;
    uint16_t total_chunks = (uint16_t)((comp_len + CHUNK_PAYLOAD - 1) / CHUNK_PAYLOAD);
    if (total_chunks == 0 || total_chunks >= MAX_CHUNKS) return;

    uint8_t pkt[sizeof(PacketHeader) + CRYPTO_NONCE_LEN + CHUNK_PAYLOAD + CRYPTO_TAG_LEN];
    PacketHeader *hdr = (PacketHeader *)pkt;
    hdr->frame_id     = frame_id;
    hdr->total_chunks = total_chunks;
    hdr->width        = (uint16_t)w;
    hdr->height       = (uint16_t)h;
    hdr->codec        = codec;

    EnterCriticalSection(&g_lock);
    int n_targets = g_num_targets;
    struct sockaddr_in targets[MAX_TARGETS];
    memcpy(targets, g_targets, sizeof(struct sockaddr_in) * n_targets);
    LeaveCriticalSection(&g_lock);
    if (n_targets == 0) return;

    for (uint16_t idx = 0; idx < total_chunks; idx++) {
        uint32_t offset = (uint32_t)idx * CHUNK_PAYLOAD;
        uint32_t sz = comp_len - offset;
        if (sz > CHUNK_PAYLOAD) sz = CHUNK_PAYLOAD;

        hdr->chunk_index = idx;
        uint8_t *nonce  = pkt + sizeof(PacketHeader);
        uint8_t *cipher = nonce + CRYPTO_NONCE_LEN;
        uint8_t *tag    = cipher + sz;

        if (crypto_seal(&g_crypto, (const uint8_t *)hdr, sizeof(PacketHeader),
                        comp + offset, sz, nonce, cipher, tag) != 0) {
            return;
        }

        int pkt_len = (int)(sizeof(PacketHeader) + CRYPTO_NONCE_LEN + sz + CRYPTO_TAG_LEN);
        for (int t = 0; t < n_targets; t++) {
            sendto(g_sock, (const char *)pkt, pkt_len, 0,
                   (struct sockaddr *)&targets[t], sizeof(struct sockaddr_in));
        }
        /* Micro-pacing a cada 8 pacotes para evitar buffer overflow e perda UDP */
        if ((idx & 7) == 7) {
            SwitchToThread();
        }
    }
}

/* Captura janela específica (HWND) ou retângulo de monitor via GDI preservando proporção */
static void capture_gdi_source(HWND hwnd, RECT mon_rc, int max_h, uint8_t *out_bgra, int *out_w, int *out_h) {
    int src_w = 0, src_h = 0;
    HDC hSrcDC = NULL;
    int release_hwnd = 0;

    if (hwnd && IsWindow(hwnd) && !IsIconic(hwnd)) {
        RECT rc;
        if (GetClientRect(hwnd, &rc) && (rc.right - rc.left) > 32 && (rc.bottom - rc.top) > 32) {
            src_w = rc.right - rc.left;
            src_h = rc.bottom - rc.top;
            hSrcDC = GetDC(hwnd);
            release_hwnd = 1;
        }
    }

    if (!hSrcDC) {
        src_w = mon_rc.right - mon_rc.left;
        src_h = mon_rc.bottom - mon_rc.top;
        if (src_w <= 0 || src_h <= 0) {
            src_w = GetSystemMetrics(SM_CXSCREEN);
            src_h = GetSystemMetrics(SM_CYSCREEN);
            mon_rc.left = 0; mon_rc.top = 0;
        }
        hSrcDC = GetDC(NULL);
        release_hwnd = 0;
    }

    int dst_w = 1280, dst_h = 720;
    compute_scaled_dims(src_w, src_h, max_h, &dst_w, &dst_h);
    *out_w = dst_w;
    *out_h = dst_h;

    HDC hMem = CreateCompatibleDC(hSrcDC);
    HBITMAP hBmp = CreateCompatibleBitmap(hSrcDC, dst_w, dst_h);
    HGDIOBJ old = SelectObject(hMem, hBmp);
    SetStretchBltMode(hMem, HALFTONE);

    if (release_hwnd) {
        HDC hFull = CreateCompatibleDC(hSrcDC);
        HBITMAP hBmpFull = CreateCompatibleBitmap(hSrcDC, src_w, src_h);
        HGDIOBJ oldFull = SelectObject(hFull, hBmpFull);
        if (!PrintWindow(hwnd, hFull, PW_RENDERFULLCONTENT)) {
            BitBlt(hFull, 0, 0, src_w, src_h, hSrcDC, 0, 0, SRCCOPY);
        }
        StretchBlt(hMem, 0, 0, dst_w, dst_h, hFull, 0, 0, src_w, src_h, SRCCOPY);
        SelectObject(hFull, oldFull);
        DeleteObject(hBmpFull);
        DeleteDC(hFull);
    } else {
        StretchBlt(hMem, 0, 0, dst_w, dst_h, hSrcDC, mon_rc.left, mon_rc.top, src_w, src_h, SRCCOPY);
    }

    SelectObject(hMem, old);

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = dst_w;
    bmi.bmiHeader.biHeight      = -dst_h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(hMem, hBmp, 0, dst_h, out_bgra, &bmi, DIB_RGB_COLORS);

    DeleteObject(hBmp);
    DeleteDC(hMem);
    if (release_hwnd) ReleaseDC(hwnd, hSrcDC);
    else ReleaseDC(NULL, hSrcDC);
}

static DWORD WINAPI capture_thread_proc(LPVOID param) {
    (void)param;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    int max_h   = (int)InterlockedCompareExchange(&g_cfg_max_h, 0, 0);
    int fps     = (int)InterlockedCompareExchange(&g_cfg_fps, 0, 0);
    int bitrate = (int)InterlockedCompareExchange(&g_cfg_bitrate, 0, 0);
    int mon_idx = (int)InterlockedCompareExchange(&g_cfg_monitor, 0, 0);
    HWND target_hwnd = (HWND)InterlockedCompareExchangePointer((PVOID volatile *)&g_cfg_hwnd, NULL, NULL);

    ID3D11Device           *device  = NULL;
    ID3D11DeviceContext    *context = NULL;
    IDXGIOutputDuplication *dupl    = NULL;
    ID3D11Texture2D        *staging = NULL;
    RECT                    mon_rc  = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};

    if (!target_hwnd) {
        D3D_FEATURE_LEVEL fl;
        if (SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                        NULL, 0, D3D11_SDK_VERSION, &device, &fl, &context))) {
            IDXGIDevice *dxgi_dev = NULL;
            if (SUCCEEDED(ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_dev))) {
                IDXGIAdapter *adapter = NULL;
                if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter))) {
                    IDXGIOutput *output = NULL;
                    if (FAILED(IDXGIAdapter_EnumOutputs(adapter, (UINT)mon_idx, &output))) {
                        IDXGIAdapter_EnumOutputs(adapter, 0, &output);
                    }
                    if (output) {
                        DXGI_OUTPUT_DESC odesc;
                        if (SUCCEEDED(IDXGIOutput_GetDesc(output, &odesc))) {
                            mon_rc = odesc.DesktopCoordinates;
                        }
                        IDXGIOutput1 *output1 = NULL;
                        if (SUCCEEDED(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1))) {
                            IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)device, &dupl);
                            IDXGIOutput1_Release(output1);
                        }
                        IDXGIOutput_Release(output);
                    }
                    IDXGIAdapter_Release(adapter);
                }
                IDXGIDevice_Release(dxgi_dev);
            }
        }
    }

    int cur_w = 1280, cur_h = 720;
    capture_gdi_source(target_hwnd, mon_rc, max_h, g_cap_bgra, &cur_w, &cur_h);

    LARGE_INTEGER freq, t_start, t_now;
    QueryPerformanceFrequency(&freq);
    uint32_t frame_id = 1;
    DWORD last_keyframe_ts = 0;

    while (InterlockedCompareExchange(&g_running, 0, 0) &&
           InterlockedCompareExchange(&g_capturing, 0, 0)) {
        QueryPerformanceCounter(&t_start);

        int got_dxgi = 0;
        if (dupl && !target_hwnd) {
            IDXGIResource *res = NULL;
            DXGI_OUTDUPL_FRAME_INFO fi;
            HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(dupl, 6, &fi, &res);
            if (SUCCEEDED(hr) && res) {
                ID3D11Texture2D *tex = NULL;
                if (SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
                    D3D11_TEXTURE2D_DESC desc;
                    ID3D11Texture2D_GetDesc(tex, &desc);
                    compute_scaled_dims((int)desc.Width, (int)desc.Height, max_h, &cur_w, &cur_h);
                    if (!staging) {
                        D3D11_TEXTURE2D_DESC sdesc = desc;
                        sdesc.Usage          = D3D11_USAGE_STAGING;
                        sdesc.BindFlags      = 0;
                        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        sdesc.MiscFlags      = 0;
                        ID3D11Device_CreateTexture2D(device, &sdesc, NULL, &staging);
                    }
                    if (staging) {
                        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
                        D3D11_MAPPED_SUBRESOURCE map;
                        if (SUCCEEDED(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map))) {
                            scale_bgra((const uint8_t *)map.pData, (int)desc.Width, (int)desc.Height,
                                       (int)map.RowPitch, g_cap_bgra, cur_w, cur_h);
                            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
                            got_dxgi = 1;
                        }
                    }
                    ID3D11Texture2D_Release(tex);
                }
                IDXGIResource_Release(res);
                IDXGIOutputDuplication_ReleaseFrame(dupl);
            } else if (hr == DXGI_ERROR_ACCESS_LOST) {
                IDXGIOutputDuplication_Release(dupl);
                dupl = NULL;
            }
        }

        if (!got_dxgi && (target_hwnd || !dupl || (frame_id % 4 == 0))) {
            capture_gdi_source(target_hwnd, mon_rc, max_h, g_cap_bgra, &cur_w, &cur_h);
        }

        FrameCallback cb = g_callback;
        if (cb) {
            cb(0, g_cap_bgra, cur_w, cur_h, cur_w * 4);
        }

        /* Detecção rápida de tela estática para economizar banda e evitar saturação da rede */
        DWORD now_tick = GetTickCount();
        int is_different = 0;
        if (!g_prev_bgra || (now_tick - last_keyframe_ts) >= 1000) {
            is_different = 1;
        } else {
            const uint64_t *cur64 = (const uint64_t *)g_cap_bgra;
            const uint64_t *prev64 = (const uint64_t *)g_prev_bgra;
            int total_words = (cur_w * cur_h * 4) / 8;
            int diff_count = 0;
            for (int i = 0; i < total_words; i += 8) {
                if (cur64[i] != prev64[i]) {
                    diff_count++;
                    if (diff_count > 4) {
                        is_different = 1;
                        break;
                    }
                }
            }
        }

        if (is_different) {
            if (g_prev_bgra) {
                memcpy(g_prev_bgra, g_cap_bgra, cur_w * cur_h * 4);
            }
            last_keyframe_ts = now_tick;

            uint32_t comp_len = compress_rle(g_cap_bgra, cur_w, cur_h, bitrate, g_cap_comp);
            if (comp_len > 0) {
                send_encoded_frame(frame_id++, g_cap_comp, comp_len, cur_w, cur_h, CODEC_RLE);
            }
        }

        double target_ms = 1000.0 / (double)(fps > 0 ? fps : 60);
        QueryPerformanceCounter(&t_now);
        double elapsed_ms = (double)(t_now.QuadPart - t_start.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (elapsed_ms < target_ms) {
            DWORD wait_ms = (DWORD)(target_ms - elapsed_ms);
            if (wait_ms > 0) Sleep(wait_ms);
        }
    }

    if (staging) ID3D11Texture2D_Release(staging);
    if (dupl)    IDXGIOutputDuplication_Release(dupl);
    if (context) ID3D11DeviceContext_Release(context);
    if (device)  ID3D11Device_Release(device);
    MFShutdown();
    CoUninitialize();
    return 0;
}

static int find_stream_slot(uint32_t sender_id, RxStream **out_stream) {
    DWORD now = GetTickCount();
    int oldest_idx = 0;

    for (int i = 0; i < MAX_STREAMS; i++) {
        if (g_rx_streams[i].sender_id == sender_id) {
            *out_stream = &g_rx_streams[i];
            return i + 1;
        }
        if (g_rx_streams[i].last_rx < g_rx_streams[oldest_idx].last_rx) {
            oldest_idx = i;
        }
    }

    RxStream *s = &g_rx_streams[oldest_idx];
    s->sender_id              = sender_id;
    s->last_rx                = now;
    s->last_rendered_frame_id = 0;
    for (int k = 0; k < NUM_FRAME_SLOTS; k++) {
        s->slots[k].frame_id        = 0;
        s->slots[k].received_chunks = 0;
        s->slots[k].total_chunks    = 0;
        s->slots[k].comp_size       = 0;
        s->slots[k].start_ts        = now;
        memset(s->slots[k].chunk_mask, 0, MAX_CHUNKS);
    }
    *out_stream = s;
    return oldest_idx + 1;
}

static DWORD WINAPI rx_thread_proc(LPVOID param) {
    (void)param;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    uint8_t pkt[sizeof(PacketHeader) + CRYPTO_NONCE_LEN + CHUNK_PAYLOAD + CRYPTO_TAG_LEN + 32];
    uint8_t plain_chunk[CHUNK_PAYLOAD];
    struct sockaddr_in from_addr;

    while (InterlockedCompareExchange(&g_running, 0, 0)) {
        int from_len = sizeof(from_addr);
        int n = recvfrom(g_sock, (char *)pkt, sizeof(pkt), 0,
                         (struct sockaddr *)&from_addr, &from_len);
        if (n == 4) {
            if (memcmp(pkt, "PING", 4) == 0) {
                sendto(g_sock, "PONG", 4, 0, (struct sockaddr *)&from_addr, sizeof(from_addr));
                if (!is_known_target_ip(from_addr.sin_addr.s_addr)) {
                    EnterCriticalSection(&g_lock);
                    if (g_num_targets < MAX_TARGETS) {
                        g_targets[g_num_targets++] = from_addr;
                    }
                    LeaveCriticalSection(&g_lock);
                }
                continue;
            } else if (memcmp(pkt, "PONG", 4) == 0) {
                if (!is_known_target_ip(from_addr.sin_addr.s_addr)) {
                    EnterCriticalSection(&g_lock);
                    if (g_num_targets < MAX_TARGETS) {
                        g_targets[g_num_targets++] = from_addr;
                    }
                    LeaveCriticalSection(&g_lock);
                }
                continue;
            }
        } else if (n <= 0) {
            continue;
        }

        int min_len = (int)(sizeof(PacketHeader) + CRYPTO_NONCE_LEN + CRYPTO_TAG_LEN);
        if (n <= min_len) continue;

        uint32_t cipher_len = (uint32_t)(n - min_len);
        if (cipher_len == 0 || cipher_len > CHUNK_PAYLOAD) continue;

        PacketHeader *hdr = (PacketHeader *)pkt;
        uint8_t *nonce    = pkt + sizeof(PacketHeader);
        uint8_t *cipher   = nonce + CRYPTO_NONCE_LEN;
        uint8_t *tag      = cipher + cipher_len;
        uint32_t sid      = 0;

        /* Abre e autentica o pacote via AES-256-GCM */
        int rc = crypto_open(&g_crypto, (const uint8_t *)hdr, sizeof(PacketHeader),
                             nonce, cipher, cipher_len, tag, plain_chunk, &sid);
        if (rc != 0 || sid == 0) continue;

        /* Se autenticado com sucesso e o IP ainda não constava, registra como alvo dinamicamente (NAT punch-through) */
        if (!is_known_target_ip(from_addr.sin_addr.s_addr)) {
            EnterCriticalSection(&g_lock);
            if (g_num_targets < MAX_TARGETS) {
                g_targets[g_num_targets++] = from_addr;
            }
            LeaveCriticalSection(&g_lock);
        }

        if (hdr->total_chunks == 0 || hdr->total_chunks >= MAX_CHUNKS ||
            hdr->chunk_index >= hdr->total_chunks ||
            hdr->width < 64 || hdr->width > MAX_W ||
            hdr->height < 64 || hdr->height > MAX_H ||
            (hdr->width & 1) || (hdr->height & 1)) {
            continue;
        }

        uint32_t offset = (uint32_t)hdr->chunk_index * CHUNK_PAYLOAD;
        if (offset + cipher_len > MAX_COMP_BYTES) continue;

        DWORD now = GetTickCount();
        RxStream *s = NULL;
        int stream_id = find_stream_slot(sid, &s);
        s->last_rx = now;

        /* Roteia para o slot correto no ring buffer (NUM_FRAME_SLOTS = 4) */
        int slot_idx = (int)(hdr->frame_id % NUM_FRAME_SLOTS);
        FrameSlot *slot = &s->slots[slot_idx];

        if (slot->frame_id != hdr->frame_id || (now - slot->start_ts) > FRAME_TIMEOUT_MS) {
            /* Descarta chunk desatualizado se for de frame anterior */
            if (hdr->frame_id < slot->frame_id && (slot->frame_id - hdr->frame_id) < 1000) {
                continue;
            }
            slot->frame_id        = hdr->frame_id;
            slot->total_chunks    = hdr->total_chunks;
            slot->received_chunks = 0;
            slot->width           = hdr->width;
            slot->height          = hdr->height;
            slot->codec           = hdr->codec;
            slot->comp_size       = 0;
            slot->start_ts        = now;
            memset(slot->chunk_mask, 0, hdr->total_chunks);
        }

        if (hdr->total_chunks != slot->total_chunks || hdr->width != slot->width || hdr->height != slot->height) {
            continue;
        }

        uint16_t idx = hdr->chunk_index;
        if (!slot->chunk_mask[idx]) {
            memcpy(slot->comp_buf + offset, plain_chunk, cipher_len);
            slot->chunk_mask[idx] = 1;
            slot->received_chunks++;
            if (offset + cipher_len > slot->comp_size) {
                slot->comp_size = offset + cipher_len;
            }
        }

        /* Se todos os chunks do slot foram recebidos com sucesso */
        if (slot->received_chunks == slot->total_chunks) {
            if (slot->frame_id >= s->last_rendered_frame_id || (s->last_rendered_frame_id - slot->frame_id) > 10000) {
                s->last_rendered_frame_id = slot->frame_id;
                int decoded_ok = 0;

                if (slot->codec == CODEC_H264) {
                    if (!s->h264_dec.ready || s->h264_dec.width != slot->width || s->h264_dec.height != slot->height) {
                        mf_init_h264_decoder(&s->h264_dec, slot->width, slot->height);
                    }
                    if (s->h264_dec.ready) {
                        uint32_t nv12_expected = (uint32_t)(slot->width * slot->height * 3 / 2);
                        uint32_t out_nv12 = mf_process_sample(&s->h264_dec, slot->comp_buf, slot->comp_size,
                                                              0, 166666, s->nv12_buf, nv12_expected);
                        if (out_nv12 >= nv12_expected) {
                            nv12_to_bgra(s->nv12_buf, s->bgra_buf, slot->width, slot->height);
                            decoded_ok = 1;
                        }
                    }
                } else if (slot->codec == CODEC_RLE) {
                    if (decompress_rle(slot->comp_buf, slot->comp_size, s->bgra_buf, slot->width, slot->height) == 0) {
                        decoded_ok = 1;
                    }
                }

                if (decoded_ok) {
                    FrameCallback cb = g_callback;
                    if (cb) {
                        cb(stream_id, s->bgra_buf, slot->width, slot->height, slot->width * 4);
                    }
                }
            }
            slot->received_chunks = 0;
        }
    }

    for (int i = 0; i < MAX_STREAMS; i++) {
        mf_close_codec(&g_rx_streams[i].h264_dec);
    }
    MFShutdown();
    CoUninitialize();
    return 0;
}

EXPORT void video_cleanup(void);

EXPORT int video_init(int local_udp_port) {
    if (InterlockedCompareExchange(&g_running, 0, 0)) {
        video_cleanup();
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) {
        WSACleanup();
        return -2;
    }

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
    BOOL bNewBehavior = FALSE;
    DWORD dwBytesReturned = 0;
    WSAIoctl(g_sock, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);

    int rcv_buf = 8 * 1024 * 1024;
    int snd_buf = 8 * 1024 * 1024;
    DWORD rcv_timeout = 100;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv_buf, sizeof(rcv_buf));
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&snd_buf, sizeof(snd_buf));
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcv_timeout, sizeof(rcv_timeout));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port        = htons((uint16_t)local_udp_port);

    if (bind(g_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == SOCKET_ERROR) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        WSACleanup();
        return -3;
    }

    g_cap_bgra  = (uint8_t *)malloc(MAX_RAW_BYTES);
    g_cap_nv12  = (uint8_t *)malloc(MAX_PIXELS * 3 / 2);
    g_cap_comp  = (uint8_t *)malloc(MAX_COMP_BYTES);
    g_prev_bgra = (uint8_t *)malloc(MAX_RAW_BYTES);
    if (g_prev_bgra) memset(g_prev_bgra, 0, MAX_RAW_BYTES);

    memset(g_rx_streams, 0, sizeof(g_rx_streams));
    for (int i = 0; i < MAX_STREAMS; i++) {
        g_rx_streams[i].bgra_buf = (uint8_t *)malloc(MAX_RAW_BYTES);
        g_rx_streams[i].nv12_buf = (uint8_t *)malloc(MAX_PIXELS * 3 / 2);
        for (int k = 0; k < NUM_FRAME_SLOTS; k++) {
            g_rx_streams[i].slots[k].comp_buf = (uint8_t *)malloc(MAX_COMP_BYTES);
            memset(g_rx_streams[i].slots[k].chunk_mask, 0, MAX_CHUNKS);
            g_rx_streams[i].slots[k].frame_id        = 0;
            g_rx_streams[i].slots[k].received_chunks = 0;
            g_rx_streams[i].slots[k].total_chunks    = 0;
        }
    }

    InitializeCriticalSection(&g_lock);
    crypto_init_ctx(&g_crypto);
    g_num_targets = 0;
    InterlockedExchange(&g_running, 1);
    InterlockedExchange(&g_capturing, 0);

    g_rx_thread = CreateThread(NULL, 0, rx_thread_proc, NULL, 0, NULL);
    SetThreadPriority(g_rx_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    return 0;
}

EXPORT int video_set_key(const unsigned char *key32, unsigned int sender_id) {
    if (!InterlockedCompareExchange(&g_running, 0, 0) || !key32) return -1;
    return crypto_set_key(&g_crypto, key32, sender_id);
}

EXPORT int video_start_capture_ex(int max_height, int target_fps, int bitrate, uint64_t hwnd, int monitor_index) {
    if (!InterlockedCompareExchange(&g_running, 0, 0)) return -1;
    if (InterlockedCompareExchange(&g_capturing, 0, 0)) return 0;

    InterlockedExchange(&g_cfg_max_h, max_height >= 1080 ? 1080 : 720);
    InterlockedExchange(&g_cfg_fps, target_fps == 30 ? 30 : 60);
    InterlockedExchange(&g_cfg_bitrate, bitrate > 500000 ? bitrate : 4000000);
    InterlockedExchange(&g_cfg_monitor, monitor_index >= 0 ? monitor_index : 0);
    InterlockedExchangePointer((PVOID volatile *)&g_cfg_hwnd, (PVOID)(uintptr_t)hwnd);
    InterlockedExchange(&g_capturing, 1);

    g_cap_thread = CreateThread(NULL, 0, capture_thread_proc, NULL, 0, NULL);
    SetThreadPriority(g_cap_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    return 0;
}

EXPORT int video_start_capture(int target_fps, int quality) {
    int br = (quality >= 85) ? 8000000 : 4000000;
    return video_start_capture_ex(720, target_fps, br, 0, 0);
}

EXPORT void video_stop_capture(void) {
    if (!InterlockedCompareExchange(&g_capturing, 0, 0)) return;
    InterlockedExchange(&g_capturing, 0);
    if (g_cap_thread) {
        WaitForSingleObject(g_cap_thread, 1500);
        CloseHandle(g_cap_thread);
        g_cap_thread = NULL;
    }
}

EXPORT void video_add_target(const char *ip, int remote_port) {
    if (!ip || !InterlockedCompareExchange(&g_running, 0, 0)) return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)remote_port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_num_targets; i++) {
        if (g_targets[i].sin_addr.s_addr == addr.sin_addr.s_addr &&
            g_targets[i].sin_port == addr.sin_port) {
            LeaveCriticalSection(&g_lock);
            if (g_sock != INVALID_SOCKET) {
                sendto(g_sock, "PING", 4, 0, (struct sockaddr *)&addr, sizeof(addr));
            }
            return;
        }
    }
    if (g_num_targets < MAX_TARGETS) {
        g_targets[g_num_targets++] = addr;
    }
    LeaveCriticalSection(&g_lock);

    /* Envia probe UDP imediatamente para abrir NAT e firewall nos dois lados */
    if (g_sock != INVALID_SOCKET) {
        sendto(g_sock, "PING", 4, 0, (struct sockaddr *)&addr, sizeof(addr));
    }
}

EXPORT void video_remove_targets(void) {
    if (!InterlockedCompareExchange(&g_running, 0, 0)) return;
    EnterCriticalSection(&g_lock);
    g_num_targets = 0;
    LeaveCriticalSection(&g_lock);
}

EXPORT void video_set_frame_callback(FrameCallback callback) {
    g_callback = callback;
}

EXPORT void video_cleanup(void) {
    if (!InterlockedCompareExchange(&g_running, 0, 0)) return;
    video_stop_capture();
    InterlockedExchange(&g_running, 0);

    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }

    if (g_rx_thread) {
        WaitForSingleObject(g_rx_thread, 1500);
        CloseHandle(g_rx_thread);
        g_rx_thread = NULL;
    }

    free(g_cap_bgra);  g_cap_bgra = NULL;
    free(g_cap_nv12);  g_cap_nv12 = NULL;
    free(g_cap_comp);  g_cap_comp = NULL;
    free(g_prev_bgra); g_prev_bgra = NULL;

    for (int i = 0; i < MAX_STREAMS; i++) {
        free(g_rx_streams[i].bgra_buf); g_rx_streams[i].bgra_buf = NULL;
        free(g_rx_streams[i].nv12_buf); g_rx_streams[i].nv12_buf = NULL;
        for (int k = 0; k < NUM_FRAME_SLOTS; k++) {
            free(g_rx_streams[i].slots[k].comp_buf);
            g_rx_streams[i].slots[k].comp_buf = NULL;
        }
    }

    crypto_free_ctx(&g_crypto);
    DeleteCriticalSection(&g_lock);
    WSACleanup();
}
