/*
 * Motor Multiplataforma de Compartilhamento de Tela / Janela para Linux (X11 + XShm)
 * - Captura: X11 com extensão MIT-SHM (X Shared Memory - XShmGetImage) para consumo mínimo de CPU
 *   mesmo em hardware antigo (ex: Intel Core 2 Duo com gráficos integrados), com fallback seguro
 *   para XGetImage caso XShm não esteja disponível.
 * - Preservação de Proporção (16:9, 16:10, Ultrawide 21:9/32:9, janelas arbitrárias)
 * - Protocolo UDP: Idêntico ao video_engine.c do Windows (PacketHeader compactado + AES-256-GCM)
 *   na porta 45002, garantindo interoperabilidade total Windows <-> Linux.
 * - Criptografia: AES-256-GCM via OpenSSL (libcrypto - EVP_aes_256_gcm)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include "../audio_engine/c_crypto.h"

#define EXPORT __attribute__((visibility("default")))

#define MAX_W            2560
#define MAX_H            1080
#define MAX_PIXELS       (MAX_W * MAX_H)
#define MAX_RAW_BYTES    (MAX_PIXELS * 4)
#define MAX_COMP_BYTES   (MAX_RAW_BYTES + 65536)
#define CHUNK_PAYLOAD    1340
#define MAX_CHUNKS       ((MAX_COMP_BYTES / CHUNK_PAYLOAD) + 8)
#define MAX_TARGETS      8
#define MAX_STREAMS      2
#define FRAME_TIMEOUT_MS 500

#define CODEC_RLE        0x01
#define CODEC_H264       0x02

/* Cabeçalho binário idêntico ao Windows (13 bytes, little-endian, #pragma pack(1)) */
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
    uint32_t sender_id;
    uint32_t cur_frame_id;
    uint16_t total_chunks;
    uint16_t received_chunks;
    uint16_t width;
    uint16_t height;
    uint8_t  codec;
    uint64_t last_rx;
    uint64_t frame_start_ts;
    uint8_t  chunk_mask[MAX_CHUNKS];
    uint8_t *comp_buf;
    uint32_t comp_size;
    uint8_t *bgra_buf;
} RxStream;

typedef struct {
    Display        *dpy;
    XImage         *img;
    XShmSegmentInfo shminfo;
    int             width;
    int             height;
    int             depth;
    int             attached;
} XShmCaptureCtx;

static pthread_mutex_t    g_lock = PTHREAD_MUTEX_INITIALIZER;
static CryptoContext      g_crypto;
static volatile int       g_running   = 0;
static volatile int       g_capturing = 0;

/* Parâmetros da captura selecionados no ScreenShareDialog */
static volatile int       g_cfg_max_h   = 720;
static volatile int       g_cfg_fps     = 60;
static volatile int       g_cfg_bitrate = 4000000;
static volatile int       g_cfg_monitor = 0;
static volatile uint64_t  g_cfg_hwnd    = 0;

static int                g_sock = -1;
static struct sockaddr_in g_targets[MAX_TARGETS];
static int                g_num_targets = 0;

static pthread_t          g_rx_thread;
static pthread_t          g_cap_thread;
static int                g_rx_started  = 0;
static int                g_cap_started = 0;
static FrameCallback      g_callback    = NULL;
static RxStream           g_rx_streams[MAX_STREAMS];

static uint8_t           *g_cap_bgra = NULL;
static uint8_t           *g_cap_comp = NULL;

static volatile int       g_x11_err_flag = 0;

static int x11_ignore_error(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    (void)ev;
    g_x11_err_flag = 1;
    return 0;
}

static uint64_t get_tick_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int is_known_target_ip(uint32_t ip_addr) {
    int ok = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_num_targets; i++) {
        if (g_targets[i].sin_addr.s_addr == ip_addr) {
            ok = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
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
    int max_w = (max_h == 1080) ? 2560 : 1720;

    double scale_h = (src_h > max_h) ? ((double)max_h / (double)src_h) : 1.0;
    double scale_w = (src_w > max_w) ? ((double)max_w / (double)src_w) : 1.0;
    double scale = (scale_h < scale_w) ? scale_h : scale_w;

    int w = ((int)(src_w * scale + 0.5)) & ~1;
    int h = ((int)(src_h * scale + 0.5)) & ~1;
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    if (w > MAX_W) w = MAX_W;
    if (h > MAX_H) h = MAX_H;
    *out_w = w;
    *out_h = h;
}

/* Codificador RLE rápido e leve para CPUs antigas (100% compatível com video_engine.c do Windows) */
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

/* Decodificador RLE rápido (idêntico ao video_engine.c do Windows) */
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

/* Converte e redimensiona XImage (32-bit ou 24-bit ZPixmap) para BGRA (Format_ARGB32) */
static void scale_ximage_to_bgra(const XImage *img, int src_w, int src_h,
                                 uint8_t *dst_bgra, int dst_w, int dst_h) {
    if (!img || !img->data) return;
    int bpp = img->bits_per_pixel;
    int stride = img->bytes_per_line;
    const uint8_t *src_base = (const uint8_t *)img->data;

    if (bpp == 32) {
        for (int y = 0; y < dst_h; y++) {
            int sy = (y * src_h) / dst_h;
            const uint32_t *src_row = (const uint32_t *)(src_base + sy * stride);
            uint32_t *dst_row = (uint32_t *)(dst_bgra + y * dst_w * 4);
            for (int x = 0; x < dst_w; x++) {
                int sx = (x * src_w) / dst_w;
                dst_row[x] = src_row[sx] | 0xFF000000u;
            }
        }
    } else if (bpp == 24) {
        for (int y = 0; y < dst_h; y++) {
            int sy = (y * src_h) / dst_h;
            const uint8_t *src_row = src_base + sy * stride;
            uint32_t *dst_row = (uint32_t *)(dst_bgra + y * dst_w * 4);
            for (int x = 0; x < dst_w; x++) {
                int sx = ((x * src_w) / dst_w) * 3;
                uint32_t b = src_row[sx];
                uint32_t g = src_row[sx + 1];
                uint32_t r = src_row[sx + 2];
                dst_row[x] = b | (g << 8) | (r << 16) | 0xFF000000u;
            }
        }
    } else {
        for (int y = 0; y < dst_h; y++) {
            int sy = (y * src_h) / dst_h;
            uint32_t *dst_row = (uint32_t *)(dst_bgra + y * dst_w * 4);
            for (int x = 0; x < dst_w; x++) {
                int sx = (x * src_w) / dst_w;
                unsigned long px = XGetPixel((XImage *)img, sx, sy);
                dst_row[x] = (uint32_t)px | 0xFF000000u;
            }
        }
    }
}

/* Libera segmento XShm sem deixar memória compartilhada órfã */
static void xshm_cleanup_ctx(XShmCaptureCtx *ctx) {
    if (!ctx || !ctx->dpy) return;
    if (ctx->attached) {
        XShmDetach(ctx->dpy, &ctx->shminfo);
        XSync(ctx->dpy, False);
        ctx->attached = 0;
    }
    if (ctx->img) {
        XDestroyImage(ctx->img);
        ctx->img = NULL;
    }
    if (ctx->shminfo.shmaddr && ctx->shminfo.shmaddr != (char *)-1) {
        shmdt(ctx->shminfo.shmaddr);
        ctx->shminfo.shmaddr = NULL;
    }
    ctx->width = 0;
    ctx->height = 0;
}

/* Inicializa ou recria segmento XShm quando a resolução da tela/janela muda */
static int xshm_ensure_ctx(XShmCaptureCtx *ctx, Display *dpy, Visual *vis, int depth, int w, int h) {
    if (ctx->attached && ctx->img && ctx->width == w && ctx->height == h && ctx->depth == depth) {
        return 1;
    }
    xshm_cleanup_ctx(ctx);
    ctx->dpy = dpy;

    if (!XShmQueryExtension(dpy)) return 0;

    memset(&ctx->shminfo, 0, sizeof(ctx->shminfo));
    ctx->img = XShmCreateImage(dpy, vis, (unsigned int)depth, ZPixmap, NULL,
                               &ctx->shminfo, (unsigned int)w, (unsigned int)h);
    if (!ctx->img) return 0;

    size_t shm_bytes = (size_t)ctx->img->bytes_per_line * (size_t)ctx->img->height;
    ctx->shminfo.shmid = shmget(IPC_PRIVATE, shm_bytes, IPC_CREAT | 0600);
    if (ctx->shminfo.shmid < 0) {
        XDestroyImage(ctx->img);
        ctx->img = NULL;
        return 0;
    }

    ctx->shminfo.shmaddr = ctx->img->data = (char *)shmat(ctx->shminfo.shmid, NULL, 0);
    if (ctx->shminfo.shmaddr == (char *)-1) {
        shmctl(ctx->shminfo.shmid, IPC_RMID, NULL);
        ctx->shminfo.shmaddr = NULL;
        XDestroyImage(ctx->img);
        ctx->img = NULL;
        return 0;
    }

    ctx->shminfo.readOnly = False;
    g_x11_err_flag = 0;
    Status st = XShmAttach(dpy, &ctx->shminfo);
    XSync(dpy, False);

    /* Marca IPC_RMID imediatamente após XShmAttach: o kernel Linux libera a memória
     * automaticamente assim que o processo e o servidor X desanexarem, evitando vazamento */
    shmctl(ctx->shminfo.shmid, IPC_RMID, NULL);

    if (!st || g_x11_err_flag) {
        shmdt(ctx->shminfo.shmaddr);
        ctx->shminfo.shmaddr = NULL;
        XDestroyImage(ctx->img);
        ctx->img = NULL;
        return 0;
    }

    ctx->width    = w;
    ctx->height   = h;
    ctx->depth    = depth;
    ctx->attached = 1;
    return 1;
}

static void send_encoded_frame(uint32_t frame_id, const uint8_t *comp, uint32_t comp_len,
                               int w, int h, uint8_t codec) {
    if (!g_crypto.ready || g_sock < 0) return;
    uint16_t total_chunks = (uint16_t)((comp_len + CHUNK_PAYLOAD - 1) / CHUNK_PAYLOAD);
    if (total_chunks == 0 || total_chunks >= MAX_CHUNKS) return;

    uint8_t pkt[sizeof(PacketHeader) + CRYPTO_NONCE_LEN + CHUNK_PAYLOAD + CRYPTO_TAG_LEN];
    PacketHeader *hdr = (PacketHeader *)pkt;
    hdr->frame_id     = frame_id;
    hdr->total_chunks = total_chunks;
    hdr->width        = (uint16_t)w;
    hdr->height       = (uint16_t)h;
    hdr->codec        = codec;

    pthread_mutex_lock(&g_lock);
    int n_targets = g_num_targets;
    struct sockaddr_in targets[MAX_TARGETS];
    memcpy(targets, g_targets, sizeof(struct sockaddr_in) * (size_t)n_targets);
    pthread_mutex_unlock(&g_lock);
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

        size_t pkt_len = sizeof(PacketHeader) + CRYPTO_NONCE_LEN + sz + CRYPTO_TAG_LEN;
        for (int t = 0; t < n_targets; t++) {
            sendto(g_sock, pkt, pkt_len, MSG_NOSIGNAL,
                   (struct sockaddr *)&targets[t], sizeof(struct sockaddr_in));
        }
        if ((idx & 31) == 31) {
            sched_yield();
        }
    }
}

/* Thread de captura X11 + XShmGetImage */
static void *capture_thread_proc(void *param) {
    (void)param;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        __atomic_store_n(&g_capturing, 0, __ATOMIC_SEQ_CST);
        return NULL;
    }

    XSetErrorHandler(x11_ignore_error);
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    int max_h        = __atomic_load_n(&g_cfg_max_h, __ATOMIC_SEQ_CST);
    int fps          = __atomic_load_n(&g_cfg_fps, __ATOMIC_SEQ_CST);
    int bitrate      = __atomic_load_n(&g_cfg_bitrate, __ATOMIC_SEQ_CST);
    uint64_t cfg_xid = __atomic_load_n(&g_cfg_hwnd, __ATOMIC_SEQ_CST);

    XShmCaptureCtx shm_ctx;
    memset(&shm_ctx, 0, sizeof(shm_ctx));

    uint32_t frame_id = 1;
    int target_fps = (fps > 0 && fps <= 60) ? fps : 30;
    int frame_interval_ms = 1000 / target_fps;

    while (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST) &&
           __atomic_load_n(&g_capturing, __ATOMIC_SEQ_CST)) {
        uint64_t t_start = get_tick_ms();

        Window cap_win = root;
        if (cfg_xid != 0) {
            XWindowAttributes wa;
            g_x11_err_flag = 0;
            if (XGetWindowAttributes(dpy, (Window)cfg_xid, &wa) &&
                !g_x11_err_flag &&
                wa.map_state == IsViewable &&
                wa.width > 32 && wa.height > 32) {
                cap_win = (Window)cfg_xid;
            }
        }

        XWindowAttributes attr;
        g_x11_err_flag = 0;
        if (!XGetWindowAttributes(dpy, cap_win, &attr) || g_x11_err_flag ||
            attr.width <= 0 || attr.height <= 0) {
            cap_win = root;
            if (!XGetWindowAttributes(dpy, root, &attr)) {
                sleep_ms(30);
                continue;
            }
        }

        int src_w = attr.width;
        int src_h = attr.height;
        int cur_w = 1280, cur_h = 720;
        compute_scaled_dims(src_w, src_h, max_h, &cur_w, &cur_h);

        int captured_ok = 0;
        if (xshm_ensure_ctx(&shm_ctx, dpy, attr.visual, attr.depth, src_w, src_h)) {
            g_x11_err_flag = 0;
            if (XShmGetImage(dpy, cap_win, shm_ctx.img, 0, 0, AllPlanes) && !g_x11_err_flag) {
                scale_ximage_to_bgra(shm_ctx.img, src_w, src_h, g_cap_bgra, cur_w, cur_h);
                captured_ok = 1;
            }
        }

        /* Fallback seguro para XGetImage caso janela esteja parcialmente fora da tela ou sem XShm */
        if (!captured_ok) {
            g_x11_err_flag = 0;
            XImage *fallback_img = XGetImage(dpy, cap_win, 0, 0,
                                             (unsigned int)src_w, (unsigned int)src_h,
                                             AllPlanes, ZPixmap);
            if (fallback_img && !g_x11_err_flag) {
                scale_ximage_to_bgra(fallback_img, src_w, src_h, g_cap_bgra, cur_w, cur_h);
                XDestroyImage(fallback_img);
                captured_ok = 1;
            }
        }

        if (captured_ok) {
            FrameCallback cb = g_callback;
            if (cb) {
                cb(0, g_cap_bgra, cur_w, cur_h, cur_w * 4);
            }

            uint32_t comp_len = compress_rle(g_cap_bgra, cur_w, cur_h, bitrate, g_cap_comp);
            if (comp_len > 0) {
                send_encoded_frame(frame_id++, g_cap_comp, comp_len, cur_w, cur_h, CODEC_RLE);
            }
        }

        uint64_t elapsed = get_tick_ms() - t_start;
        if ((int)elapsed < frame_interval_ms) {
            sleep_ms(frame_interval_ms - (int)elapsed);
        }
    }

    xshm_cleanup_ctx(&shm_ctx);
    XCloseDisplay(dpy);
    return NULL;
}

static int find_stream_slot(uint32_t sender_id, RxStream **out_stream) {
    uint64_t now = get_tick_ms();
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
    s->sender_id       = sender_id;
    s->cur_frame_id    = 0;
    s->total_chunks    = 0;
    s->received_chunks = 0;
    s->last_rx         = now;
    s->frame_start_ts  = now;
    *out_stream = s;
    return oldest_idx + 1;
}

/* Thread de recepção UDP: remonta os fragmentos, decodifica e entrega ao Python (video_view.py) */
static void *rx_thread_proc(void *param) {
    (void)param;
    uint8_t pkt[sizeof(PacketHeader) + CRYPTO_NONCE_LEN + CHUNK_PAYLOAD + CRYPTO_TAG_LEN + 32];
    uint8_t plain_chunk[CHUNK_PAYLOAD];
    struct sockaddr_in from_addr;

    while (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(g_sock, pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&from_addr, &from_len);
        ssize_t min_len = (ssize_t)(sizeof(PacketHeader) + CRYPTO_NONCE_LEN + CRYPTO_TAG_LEN);
        if (n <= min_len) continue;
        if (!is_known_target_ip(from_addr.sin_addr.s_addr)) continue;

        uint32_t cipher_len = (uint32_t)(n - min_len);
        if (cipher_len == 0 || cipher_len > CHUNK_PAYLOAD) continue;

        PacketHeader *hdr = (PacketHeader *)pkt;
        uint8_t *nonce    = pkt + sizeof(PacketHeader);
        uint8_t *cipher   = nonce + CRYPTO_NONCE_LEN;
        uint8_t *tag      = cipher + cipher_len;
        uint32_t sid      = 0;

        if (crypto_open(&g_crypto, (const uint8_t *)hdr, sizeof(PacketHeader),
                        nonce, cipher, cipher_len, tag, plain_chunk, &sid) != 0 || sid == 0) {
            continue;
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

        uint64_t now = get_tick_ms();
        RxStream *s = NULL;
        int stream_id = find_stream_slot(sid, &s);
        s->last_rx = now;

        if (hdr->frame_id != s->cur_frame_id || (now - s->frame_start_ts) > FRAME_TIMEOUT_MS) {
            if (hdr->frame_id < s->cur_frame_id && (s->cur_frame_id - hdr->frame_id) < 1000) {
                continue;
            }
            s->cur_frame_id    = hdr->frame_id;
            s->total_chunks    = hdr->total_chunks;
            s->received_chunks = 0;
            s->width           = hdr->width;
            s->height          = hdr->height;
            s->codec           = hdr->codec;
            s->comp_size       = 0;
            s->frame_start_ts  = now;
            memset(s->chunk_mask, 0, hdr->total_chunks);
        }

        if (hdr->total_chunks != s->total_chunks || hdr->width != s->width || hdr->height != s->height) {
            continue;
        }

        uint16_t idx = hdr->chunk_index;
        if (!s->chunk_mask[idx]) {
            memcpy(s->comp_buf + offset, plain_chunk, cipher_len);
            s->chunk_mask[idx] = 1;
            s->received_chunks++;
            if (offset + cipher_len > s->comp_size) {
                s->comp_size = offset + cipher_len;
            }
        }

        if (s->received_chunks == s->total_chunks) {
            int decoded_ok = 0;
            if (s->codec == CODEC_RLE) {
                if (decompress_rle(s->comp_buf, s->comp_size, s->bgra_buf, s->width, s->height) == 0) {
                    decoded_ok = 1;
                }
            }

            if (decoded_ok) {
                FrameCallback cb = g_callback;
                if (cb) {
                    cb(stream_id, s->bgra_buf, s->width, s->height, s->width * 4);
                }
            }
            s->received_chunks = 0;
        }
    }
    return NULL;
}

EXPORT void video_cleanup(void);

EXPORT int video_init(int local_udp_port) {
    if (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
        video_cleanup();
    }

    XInitThreads();

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock < 0) return -2;

    int reuse = 1;
    int rcv_buf = 8 * 1024 * 1024;
    int snd_buf = 8 * 1024 * 1024;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };

    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &rcv_buf, sizeof(rcv_buf));
    setsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, &snd_buf, sizeof(snd_buf));
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port        = htons((uint16_t)local_udp_port);

    if (bind(g_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(g_sock);
        g_sock = -1;
        return -3;
    }

    g_cap_bgra = (uint8_t *)malloc(MAX_RAW_BYTES);
    g_cap_comp = (uint8_t *)malloc(MAX_COMP_BYTES);
    if (!g_cap_bgra || !g_cap_comp) {
        free(g_cap_bgra); g_cap_bgra = NULL;
        free(g_cap_comp); g_cap_comp = NULL;
        close(g_sock); g_sock = -1;
        return -4;
    }

    memset(g_rx_streams, 0, sizeof(g_rx_streams));
    for (int i = 0; i < MAX_STREAMS; i++) {
        g_rx_streams[i].comp_buf = (uint8_t *)malloc(MAX_COMP_BYTES);
        g_rx_streams[i].bgra_buf = (uint8_t *)malloc(MAX_RAW_BYTES);
    }

    pthread_mutex_init(&g_lock, NULL);
    crypto_init_ctx(&g_crypto);
    g_num_targets = 0;
    __atomic_store_n(&g_running, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_capturing, 0, __ATOMIC_SEQ_CST);

    pthread_create(&g_rx_thread, NULL, rx_thread_proc, NULL);
    g_rx_started = 1;
    return 0;
}

EXPORT int video_set_key(const unsigned char *key32, unsigned int sender_id) {
    if (!__atomic_load_n(&g_running, __ATOMIC_SEQ_CST) || !key32) return -1;
    return crypto_set_key(&g_crypto, key32, sender_id);
}

EXPORT int video_start_capture_ex(int max_height, int target_fps, int bitrate, uint64_t hwnd, int monitor_index) {
    if (!__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) return -1;
    if (__atomic_load_n(&g_capturing, __ATOMIC_SEQ_CST)) return 0;

    __atomic_store_n(&g_cfg_max_h, max_height >= 1080 ? 1080 : 720, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_cfg_fps, target_fps == 30 ? 30 : 60, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_cfg_bitrate, bitrate > 500000 ? bitrate : 4000000, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_cfg_monitor, monitor_index >= 0 ? monitor_index : 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_cfg_hwnd, hwnd, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_capturing, 1, __ATOMIC_SEQ_CST);

    pthread_create(&g_cap_thread, NULL, capture_thread_proc, NULL);
    g_cap_started = 1;
    return 0;
}

EXPORT int video_start_capture(int target_fps, int quality) {
    int br = (quality >= 85) ? 8000000 : 4000000;
    return video_start_capture_ex(720, target_fps, br, 0, 0);
}

EXPORT void video_stop_capture(void) {
    if (!__atomic_load_n(&g_capturing, __ATOMIC_SEQ_CST)) return;
    __atomic_store_n(&g_capturing, 0, __ATOMIC_SEQ_CST);
    if (g_cap_started) {
        pthread_join(g_cap_thread, NULL);
        g_cap_started = 0;
    }
}

EXPORT void video_add_target(const char *ip, int remote_port) {
    if (!ip || !__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)remote_port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_num_targets; i++) {
        if (g_targets[i].sin_addr.s_addr == addr.sin_addr.s_addr &&
            g_targets[i].sin_port == addr.sin_port) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }
    if (g_num_targets < MAX_TARGETS) {
        g_targets[g_num_targets++] = addr;
    }
    pthread_mutex_unlock(&g_lock);
}

EXPORT void video_remove_targets(void) {
    if (!__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) return;
    pthread_mutex_lock(&g_lock);
    g_num_targets = 0;
    pthread_mutex_unlock(&g_lock);
}

EXPORT void video_set_frame_callback(FrameCallback callback) {
    g_callback = callback;
}

EXPORT void video_cleanup(void) {
    if (!__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) return;
    video_stop_capture();
    __atomic_store_n(&g_running, 0, __ATOMIC_SEQ_CST);

    if (g_sock >= 0) {
        shutdown(g_sock, SHUT_RDWR);
        close(g_sock);
        g_sock = -1;
    }

    if (g_rx_started) {
        pthread_join(g_rx_thread, NULL);
        g_rx_started = 0;
    }

    free(g_cap_bgra); g_cap_bgra = NULL;
    free(g_cap_comp); g_cap_comp = NULL;
    for (int i = 0; i < MAX_STREAMS; i++) {
        free(g_rx_streams[i].comp_buf); g_rx_streams[i].comp_buf = NULL;
        free(g_rx_streams[i].bgra_buf); g_rx_streams[i].bgra_buf = NULL;
    }

    crypto_free_ctx(&g_crypto);
    pthread_mutex_destroy(&g_lock);
}
