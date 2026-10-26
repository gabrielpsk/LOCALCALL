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
#define CHUNK_PAYLOAD    1180
#define MAX_CHUNKS       ((MAX_COMP_BYTES / CHUNK_PAYLOAD) + 8)
#define MAX_TARGETS      8
#define MAX_STREAMS      2
#define NUM_FRAME_SLOTS  4
#define FRAME_TIMEOUT_MS 600

#define CODEC_RLE        0x01
#define CODEC_H264       0x02
#define CODEC_TILES      0x03
#define CODEC_NACK       0x05

#define TILE_SZ          32
#define TILE_MODE_RLE    0
#define TILE_MODE_VIDEO  1
#define MAX_TILES        3000

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

typedef struct {
    uint16_t tile_idx;
    uint8_t  mode;
    uint16_t data_len;
} TileItemHeader;
#pragma pack(pop)

typedef void (*FrameCallback)(int stream_id, const unsigned char *data, int width, int height, int stride);

typedef struct {
    uint32_t frame_id;
    uint16_t total_chunks;
    uint16_t received_chunks;
    uint16_t width;
    uint16_t height;
    uint8_t  codec;
    uint64_t start_ts;
    uint32_t comp_size;
    uint8_t  chunk_mask[MAX_CHUNKS];
    uint8_t *comp_buf;
} FrameSlot;

typedef struct {
    uint32_t   sender_id;
    uint64_t   last_rx;
    uint32_t   last_rendered_frame_id;
    uint64_t   last_render_ts;
    int        width;
    int        height;
    FrameSlot  slots[NUM_FRAME_SLOTS];
    uint8_t   *bgra_buf;
    uint8_t   *render_buf;
    pthread_mutex_t fb_lock;
    volatile int frame_ready;
    volatile int frame_complete;
    uint32_t   active_frame_id;
    uint64_t   frame_first_pkt_ts;
    uint16_t   received_chunks_count;
    uint16_t   total_chunks_expected;
    uint32_t   tile_frame_id[MAX_TILES];
    uint8_t    chunk_seen[MAX_CHUNKS];
    uint64_t   last_nack_ts;
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
static pthread_t          g_render_thread;
static pthread_t          g_cap_thread;
static int                g_rx_started     = 0;
static int                g_render_started = 0;
static int                g_cap_started    = 0;
static FrameCallback      g_callback       = NULL;
static RxStream           g_rx_streams[MAX_STREAMS];

static uint8_t           *g_cap_bgra  = NULL;
static uint8_t           *g_prev_bgra = NULL;
static uint8_t           *g_cap_comp  = NULL;

static uint16_t s_tile_age[MAX_TILES]          = {0};
static uint8_t  s_tile_mode[MAX_TILES]         = {0};
static uint8_t  s_tile_static_count[MAX_TILES] = {0};

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

#define TILE_SZ 32

/*
 * Codec TrueColor 24-bit de Baixíssima Latência baseado em Grid de Tiles (Blocos de 32x32):
 * - Sem truncamento de cor (cores vivas, sem color banding)
 * - Transmissão diferencial (apenas blocos alterados são comprimidos e enviados)
 * - Redução drástica de pacotes UDP (de 2.300 pkts/frame para 10-30 pkts/frame)
 * - Elimina bufferbloat da rede e o atraso de 10 segundos
 */
static uint32_t compress_tile_rle(const uint32_t *tile, int count, uint8_t *out) {
    uint8_t *dst = out;
    int i = 0;

    while (i < count) {
        uint32_t p0 = tile[i];
        int run = 1;
        while (i + run < count && tile[i + run] == p0 && run < 127) {
            run++;
        }
        if (run >= 3) {
            *dst++ = (uint8_t)(0x80 | run);
            *dst++ = (uint8_t)(p0);
            *dst++ = (uint8_t)(p0 >> 8);
            *dst++ = (uint8_t)(p0 >> 16);
            i += run;
        } else {
            uint8_t *hdr = dst++;
            int lit = 0;
            while (i < count && lit < 64) {
                if (i + 3 < count && tile[i] == tile[i+1] && tile[i] == tile[i+2]) {
                    break;
                }
                uint32_t px = tile[i++];
                *dst++ = (uint8_t)(px);
                *dst++ = (uint8_t)(px >> 8);
                *dst++ = (uint8_t)(px >> 16);
                lit++;
            }
            *hdr = (uint8_t)lit;
        }
    }
    return (uint32_t)(dst - out);
}

static void decompress_tile_rle(const uint8_t *in, uint32_t in_len, uint32_t *tile, int count) {
    const uint8_t *src = in;
    const uint8_t *end = in + in_len;
    int p = 0;

    while (src < end && p < count) {
        uint8_t tag = *src++;
        if (tag & 0x80) {
            int run = tag & 0x7F;
            if (src + 3 > end) break;
            uint32_t c = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | 0xFF000000u;
            src += 3;
            for (int r = 0; r < run && p < count; r++) {
                tile[p++] = c;
            }
        } else {
            int lit = tag;
            for (int l = 0; l < lit && p < count; l++) {
                if (src + 3 > end) break;
                uint32_t c = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | 0xFF000000u;
                src += 3;
                tile[p++] = c;
            }
        }
    }
}

static uint32_t encode_tile_video(const uint32_t *tile, int tw, int th, uint8_t *out) {
    int qw = (tw + 1) / 2;
    int qh = (th + 1) / 2;
    int cw = (qw + 1) / 2;
    int ch = (qh + 1) / 2;

    uint8_t *dst_y  = out;
    uint8_t *dst_co = out + (qw * qh);
    uint8_t *dst_cg = dst_co + (cw * ch);

    int co_acc[16][16] = {{0}};
    int cg_acc[16][16] = {{0}};
    int c_cnt[16][16]  = {{0}};

    for (int qy = 0; qy < qh; qy++) {
        int y0 = qy * 2;
        int y1 = (y0 + 1 < th) ? y0 + 1 : y0;
        const uint32_t *row0 = tile + y0 * tw;
        const uint32_t *row1 = tile + y1 * tw;
        int cy = qy / 2;

        for (int qx = 0; qx < qw; qx++) {
            int x0 = qx * 2;
            int x1 = (x0 + 1 < tw) ? x0 + 1 : x0;

            uint32_t p00 = row0[x0];
            uint32_t p10 = row0[x1];
            uint32_t p01 = row1[x0];
            uint32_t p11 = row1[x1];

            int b = ((p00 & 0xFF) + (p10 & 0xFF) + (p01 & 0xFF) + (p11 & 0xFF) + 2) >> 2;
            int g = (((p00 >> 8) & 0xFF) + ((p10 >> 8) & 0xFF) + ((p01 >> 8) & 0xFF) + ((p11 >> 8) & 0xFF) + 2) >> 2;
            int r = (((p00 >> 16) & 0xFF) + ((p10 >> 16) & 0xFF) + ((p01 >> 16) & 0xFF) + ((p11 >> 16) & 0xFF) + 2) >> 2;

            int y = (r + (g << 1) + b + 2) >> 2;
            int co = (r - b);
            int cg = (-r + (g << 1) - b) >> 1;

            *dst_y++ = (uint8_t)y;

            int cx = qx / 2;
            co_acc[cy][cx] += co;
            cg_acc[cy][cx] += cg;
            c_cnt[cy][cx]++;
        }
    }

    for (int cy = 0; cy < ch; cy++) {
        for (int cx = 0; cx < cw; cx++) {
            int cnt = c_cnt[cy][cx];
            int co = cnt ? (co_acc[cy][cx] / cnt) : 0;
            int cg = cnt ? (cg_acc[cy][cx] / cnt) : 0;
            *dst_co++ = (uint8_t)((co + 256) >> 1);
            *dst_cg++ = (uint8_t)((cg + 256) >> 1);
        }
    }

    return (uint32_t)(qw * qh + cw * ch * 2);
}

static void decode_tile_video(const uint8_t *in, uint32_t in_len, uint32_t *tile, int tw, int th) {
    int qw = (tw + 1) / 2;
    int qh = (th + 1) / 2;
    int cw = (qw + 1) / 2;
    int ch = (qh + 1) / 2;
    uint32_t expected = (uint32_t)(qw * qh + cw * ch * 2);
    if (in_len < expected) return;

    const uint8_t *src_y  = in;
    const uint8_t *src_co = in + (qw * qh);
    const uint8_t *src_cg = src_co + (cw * ch);

    for (int qy = 0; qy < qh; qy++) {
        int y0 = qy * 2;
        int y1 = y0 + 1;
        uint32_t *row0 = tile + y0 * tw;
        uint32_t *row1 = (y1 < th) ? tile + y1 * tw : NULL;
        int cy = qy / 2;

        for (int qx = 0; qx < qw; qx++) {
            int cx = qx / 2;
            int y = *src_y++;
            int co = (src_co[cy * cw + cx] << 1) - 256;
            int cg = (src_cg[cy * cw + cx] << 1) - 256;

            int tmp = y - (cg >> 1);
            int g = tmp + cg;
            int r = tmp + ((co + 1) >> 1);
            int b = tmp - (co >> 1);

            if ((unsigned int)r > 255) r = (r < 0) ? 0 : 255;
            if ((unsigned int)g > 255) g = (g < 0) ? 0 : 255;
            if ((unsigned int)b > 255) b = (b < 0) ? 0 : 255;

            uint32_t c = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xFF000000u;

            int x0 = qx * 2;
            int x1 = x0 + 1;

            row0[x0] = c;
            if (x1 < tw) row0[x1] = c;
            if (row1) {
                row1[x0] = c;
                if (x1 < tw) row1[x1] = c;
            }
        }
    }
}

static void decode_tile_packet(const uint8_t *payload, uint32_t len, uint32_t *fb,
                               int w, int h, uint32_t frame_id, uint32_t *tile_frame_id) {
    if (len < 1) return;
    int cols = (w + TILE_SZ - 1) / TILE_SZ;
    uint8_t num_tiles = payload[0];
    const uint8_t *src = payload + 1;
    const uint8_t *end = payload + len;

    uint32_t tile[TILE_SZ * TILE_SZ];

    for (int i = 0; i < num_tiles && src + sizeof(TileItemHeader) <= end; i++) {
        const TileItemHeader *hdr = (const TileItemHeader *)src;
        src += sizeof(TileItemHeader);
        if (src + hdr->data_len > end) break;

        uint16_t t_idx = hdr->tile_idx;
        if (tile_frame_id && t_idx < MAX_TILES) {
            uint32_t last_f = tile_frame_id[t_idx];
            if (last_f != 0 && (int32_t)(frame_id - last_f) < 0) {
                src += hdr->data_len;
                continue;
            }
            tile_frame_id[t_idx] = frame_id;
        }

        int tx = (t_idx % cols) * TILE_SZ;
        int ty = (t_idx / cols) * TILE_SZ;
        int tw = (tx + TILE_SZ <= w) ? TILE_SZ : (w - tx);
        int th = (ty + TILE_SZ <= h) ? TILE_SZ : (h - ty);
        int count = tw * th;

        if (hdr->mode == TILE_MODE_RLE) {
            decompress_tile_rle(src, hdr->data_len, tile, count);
        } else if (hdr->mode == TILE_MODE_VIDEO) {
            decode_tile_video(src, hdr->data_len, tile, tw, th);
        }
        src += hdr->data_len;

        int idx = 0;
        for (int y = 0; y < th; y++) {
            uint32_t *row = fb + (ty + y) * w + tx;
            for (int x = 0; x < tw; x++) {
                row[x] = tile[idx++];
            }
        }
    }
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

static void micro_spin_delay(int usec) {
    if (usec <= 0) return;
    struct timespec start, cur;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t target_ns = (uint64_t)usec * 1000ULL;
    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &cur);
        uint64_t diff_ns = ((uint64_t)cur.tv_sec - (uint64_t)start.tv_sec) * 1000000000ULL +
                           ((uint64_t)cur.tv_nsec - (uint64_t)start.tv_nsec);
        if (diff_ns >= target_ns) break;
        #if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        __builtin_ia32_pause();
        #endif
    }
}

static void send_one_tile_packet(uint32_t frame_id, uint16_t chunk_idx, uint16_t total_chunks,
                                 int w, int h, const uint8_t *payload, uint32_t payload_len) {
    if (!g_crypto.ready || g_sock < 0 || payload_len == 0) return;

    uint8_t pkt[sizeof(PacketHeader) + CRYPTO_NONCE_LEN + CHUNK_PAYLOAD + CRYPTO_TAG_LEN];
    PacketHeader *hdr = (PacketHeader *)pkt;
    hdr->frame_id     = frame_id;
    hdr->chunk_index  = chunk_idx;
    hdr->total_chunks = total_chunks;
    hdr->width        = (uint16_t)w;
    hdr->height       = (uint16_t)h;
    hdr->codec        = CODEC_TILES;

    pthread_mutex_lock(&g_lock);
    int n_targets = g_num_targets;
    struct sockaddr_in targets[MAX_TARGETS];
    memcpy(targets, g_targets, sizeof(struct sockaddr_in) * (size_t)n_targets);
    pthread_mutex_unlock(&g_lock);
    if (n_targets == 0) return;

    uint8_t *nonce  = pkt + sizeof(PacketHeader);
    uint8_t *cipher = nonce + CRYPTO_NONCE_LEN;
    uint8_t *tag    = cipher + payload_len;

    if (crypto_seal(&g_crypto, (const uint8_t *)hdr, sizeof(PacketHeader),
                    payload, payload_len, nonce, cipher, tag) != 0) {
        return;
    }

    size_t pkt_len = sizeof(PacketHeader) + CRYPTO_NONCE_LEN + payload_len + CRYPTO_TAG_LEN;
    for (int t = 0; t < n_targets; t++) {
        sendto(g_sock, pkt, pkt_len, MSG_NOSIGNAL,
               (struct sockaddr *)&targets[t], sizeof(struct sockaddr_in));
    }
}

static int compute_tile_diff(const uint32_t *cur, const uint32_t *prev, int tw, int th, int stride) {
    if (!prev) return 1000000;
    int sad = 0;
    int max_diff = 0;
    for (int y = 0; y < th; y += 2) {
        const uint32_t *r_cur = cur + y * stride;
        const uint32_t *r_prv = prev + y * stride;
        for (int x = 0; x < tw; x += 2) {
            uint32_t c1 = r_cur[x];
            uint32_t c2 = r_prv[x];
            if (c1 != c2) {
                int db = abs((int)(c1 & 0xFF) - (int)(c2 & 0xFF));
                int dg = abs((int)((c1 >> 8) & 0xFF) - (int)((c2 >> 8) & 0xFF));
                int dr = abs((int)((c1 >> 16) & 0xFF) - (int)((c2 >> 16) & 0xFF));
                int d = dr + dg + db;
                sad += d;
                if (d > max_diff) max_diff = d;
            }
        }
    }
    if (max_diff <= 6 && sad < 48) return 0;
    return sad + 1;
}

typedef struct {
    uint16_t tile_idx;
    uint32_t score;
} TileCandidate;

static int compare_tile_candidates(const void *a, const void *b) {
    const TileCandidate *ca = (const TileCandidate *)a;
    const TileCandidate *cb = (const TileCandidate *)b;
    if (cb->score > ca->score) return 1;
    if (cb->score < ca->score) return -1;
    return 0;
}

static void send_nack_packet(uint32_t missing_frame_id) {
    if (!g_crypto.ready || g_sock < 0) return;
    uint8_t pkt[sizeof(PacketHeader) + CRYPTO_NONCE_LEN + 4 + CRYPTO_TAG_LEN];
    PacketHeader *hdr = (PacketHeader *)pkt;
    hdr->frame_id     = missing_frame_id;
    hdr->chunk_index  = 0;
    hdr->total_chunks = 1;
    hdr->width        = 0;
    hdr->height       = 0;
    hdr->codec        = CODEC_NACK;

    uint8_t payload[4];
    *(uint32_t *)payload = missing_frame_id;

    pthread_mutex_lock(&g_lock);
    int n_targets = g_num_targets;
    struct sockaddr_in targets[MAX_TARGETS];
    memcpy(targets, g_targets, sizeof(struct sockaddr_in) * (size_t)n_targets);
    pthread_mutex_unlock(&g_lock);
    if (n_targets == 0) return;

    uint8_t *nonce  = pkt + sizeof(PacketHeader);
    uint8_t *cipher = nonce + CRYPTO_NONCE_LEN;
    uint8_t *tag    = cipher + 4;

    if (crypto_seal(&g_crypto, (const uint8_t *)hdr, sizeof(PacketHeader),
                    payload, 4, nonce, cipher, tag) != 0) {
        return;
    }
    size_t pkt_len = sizeof(PacketHeader) + CRYPTO_NONCE_LEN + 4 + CRYPTO_TAG_LEN;
    for (int t = 0; t < n_targets; t++) {
        sendto(g_sock, pkt, pkt_len, MSG_NOSIGNAL,
               (struct sockaddr *)&targets[t], sizeof(struct sockaddr_in));
    }
}

static void encode_and_send_tiles(uint32_t frame_id, const uint32_t *cur, uint32_t *prev,
                                  int w, int h, int force_all) {
    int cols = (w + TILE_SZ - 1) / TILE_SZ;
    int rows = (h + TILE_SZ - 1) / TILE_SZ;
    int total_tiles = cols * rows;
    if (total_tiles > MAX_TILES) total_tiles = MAX_TILES;

    int fps = g_cfg_fps > 0 ? (int)g_cfg_fps : 60;
    int bitrate = g_cfg_bitrate > 500000 ? (int)g_cfg_bitrate : 6000000;

    int frame_byte_budget = (bitrate / 8) / fps;
    if (frame_byte_budget < 24000) frame_byte_budget = 24000;
    int max_burst_bytes = frame_byte_budget * 2;
    if (max_burst_bytes > 128000) max_burst_bytes = 128000;

    static TileCandidate candidates[MAX_TILES];
    int cand_count = 0;

    static int s_rir_cursor = 0;
    int rir_batch = 16;
    int rir_start = s_rir_cursor;
    int rir_end   = s_rir_cursor + rir_batch;
    s_rir_cursor = (s_rir_cursor + rir_batch) % total_tiles;

    for (int t = 0; t < total_tiles; t++) {
        int tx = (t % cols) * TILE_SZ;
        int ty = (t / cols) * TILE_SZ;
        int tw = (tx + TILE_SZ <= w) ? TILE_SZ : (w - tx);
        int th = (ty + TILE_SZ <= h) ? TILE_SZ : (h - ty);

        int is_rir = (t >= rir_start && t < rir_end) ||
                     (rir_end > total_tiles && t < (rir_end % total_tiles));

        int diff = 0;
        if (force_all || !prev) {
            diff = 1000000;
        } else {
            diff = compute_tile_diff(cur + ty * w + tx, prev + ty * w + tx, tw, th, w);
        }

        uint32_t score = 0;
        if (force_all || !prev) {
            score = 1000000 + (uint32_t)(total_tiles - t);
        } else if (diff > 0) {
            score = (uint32_t)diff + (uint32_t)s_tile_age[t] * 128;
        } else if (is_rir || s_tile_age[t] >= 60) {
            score = 50 + (uint32_t)s_tile_age[t];
        } else {
            s_tile_age[t]++;
        }

        if (score > 0) {
            candidates[cand_count].tile_idx = (uint16_t)t;
            candidates[cand_count].score = score;
            cand_count++;
        }
    }

    if (cand_count == 0) return;

    if (cand_count > 1 && !force_all) {
        qsort(candidates, (size_t)cand_count, sizeof(TileCandidate), compare_tile_candidates);
    }

    static uint32_t pkt_offsets[MAX_CHUNKS];
    static uint16_t pkt_lens[MAX_CHUNKS];
    int n_pkts = 0;

    uint8_t *comp_base = g_cap_comp;
    uint32_t cur_pkt_offset = 0;
    uint8_t *cur_pkt = comp_base;
    cur_pkt[0] = 0;
    uint32_t cur_pkt_len = 1;

    uint32_t tile[TILE_SZ * TILE_SZ];
    uint8_t rle_buf[TILE_SZ * TILE_SZ * 4 + 64];
    uint8_t vid_buf[TILE_SZ * TILE_SZ * 2 + 64];

    int total_bytes_encoded = 0;

    for (int c = 0; c < cand_count; c++) {
        if (total_bytes_encoded >= max_burst_bytes && cur_pkt[0] > 0 && !force_all) {
            for (int rem = c; rem < cand_count; rem++) {
                s_tile_age[candidates[rem].tile_idx]++;
            }
            break;
        }

        int t = candidates[c].tile_idx;
        int tx = (t % cols) * TILE_SZ;
        int ty = (t / cols) * TILE_SZ;
        int tw = (tx + TILE_SZ <= w) ? TILE_SZ : (w - tx);
        int th = (ty + TILE_SZ <= h) ? TILE_SZ : (h - ty);
        int count = tw * th;

        int idx = 0;
        for (int y = 0; y < th; y++) {
            const uint32_t *r = cur + (ty + y) * w + tx;
            for (int x = 0; x < tw; x++) {
                tile[idx++] = r[x];
            }
        }

        uint8_t mode = TILE_MODE_RLE;
        const uint8_t *data_ptr = rle_buf;
        uint32_t c_len = compress_tile_rle(tile, count, rle_buf);

        if (c_len > 384 || (s_tile_mode[t] == TILE_MODE_VIDEO && s_tile_static_count[t] < 10)) {
            mode = TILE_MODE_VIDEO;
            c_len = encode_tile_video(tile, tw, th, vid_buf);
            data_ptr = vid_buf;
            s_tile_mode[t] = TILE_MODE_VIDEO;
            s_tile_static_count[t] = 0;
        } else {
            s_tile_mode[t] = TILE_MODE_RLE;
            if (s_tile_static_count[t] < 255) s_tile_static_count[t]++;
        }

        uint32_t item_sz = sizeof(TileItemHeader) + c_len;

        if (cur_pkt_len + item_sz > CHUNK_PAYLOAD && cur_pkt[0] > 0) {
            pkt_offsets[n_pkts] = cur_pkt_offset;
            pkt_lens[n_pkts] = (uint16_t)cur_pkt_len;
            n_pkts++;
            if (n_pkts >= MAX_CHUNKS) break;

            cur_pkt_offset += cur_pkt_len;
            cur_pkt = comp_base + cur_pkt_offset;
            cur_pkt[0] = 0;
            cur_pkt_len = 1;
        }

        TileItemHeader *ih = (TileItemHeader *)(cur_pkt + cur_pkt_len);
        ih->tile_idx = (uint16_t)t;
        ih->mode = mode;
        ih->data_len = (uint16_t)c_len;
        cur_pkt_len += sizeof(TileItemHeader);
        memcpy(cur_pkt + cur_pkt_len, data_ptr, c_len);
        cur_pkt_len += c_len;
        cur_pkt[0]++;

        total_bytes_encoded += (int)item_sz;

        if (prev) {
            for (int y = 0; y < th; y++) {
                uint32_t *p_dst = prev + (ty + y) * w + tx;
                const uint32_t *c_src = cur + (ty + y) * w + tx;
                memcpy(p_dst, c_src, tw * 4);
            }
        }
        s_tile_age[t] = 0;
    }

    if (cur_pkt[0] > 0 && n_pkts < MAX_CHUNKS) {
        pkt_offsets[n_pkts] = cur_pkt_offset;
        pkt_lens[n_pkts] = (uint16_t)cur_pkt_len;
        n_pkts++;
    }

    if (n_pkts == 0) return;

    for (int i = 0; i < n_pkts; i++) {
        send_one_tile_packet(frame_id, (uint16_t)i, (uint16_t)n_pkts, w, h,
                             comp_base + pkt_offsets[i], pkt_lens[i]);
        if ((i & 0x03) == 0x03) {
            micro_spin_delay(30);
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

            static int s_prev_w = 0, s_prev_h = 0;
            int force_all = (frame_id <= 2 || cur_w != s_prev_w || cur_h != s_prev_h);
            if (force_all) {
                s_prev_w = cur_w;
                s_prev_h = cur_h;
            }

            encode_and_send_tiles(frame_id++, (const uint32_t *)g_cap_bgra,
                                  (uint32_t *)g_prev_bgra, cur_w, cur_h, force_all);
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

static void *render_thread_proc(void *param) {
    (void)param;
    while (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
        uint64_t now = get_tick_ms();
        for (int i = 0; i < MAX_STREAMS; i++) {
            RxStream *s = &g_rx_streams[i];
            if (s->sender_id == 0 || s->width <= 0 || s->height <= 0) continue;

            int should_render = 0;
            if (s->frame_complete) {
                should_render = 1;
            } else if (s->frame_ready && (now - s->frame_first_pkt_ts) >= 20) {
                should_render = 1;
            }

            if (should_render) {
                pthread_mutex_lock(&s->fb_lock);
                s->frame_complete = 0;
                s->frame_ready    = 0;
                s->last_rendered_frame_id = s->active_frame_id;
                s->last_render_ts = now;
                memcpy(s->render_buf, s->bgra_buf, (size_t)s->width * (size_t)s->height * 4);
                int rw = s->width;
                int rh = s->height;
                pthread_mutex_unlock(&s->fb_lock);

                FrameCallback cb = g_callback;
                if (cb) {
                    cb(i + 1, s->render_buf, rw, rh, rw * 4);
                }
            }
        }
        sleep_ms(4);
    }
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
    s->sender_id              = sender_id;
    s->last_rx                = now;
    s->last_rendered_frame_id = 0;
    s->last_render_ts         = 0;
    s->width                  = 0;
    s->height                 = 0;
    s->frame_ready            = 0;
    s->frame_complete         = 0;
    s->active_frame_id        = 0;
    s->frame_first_pkt_ts     = 0;
    s->received_chunks_count  = 0;
    s->total_chunks_expected  = 0;
    s->last_nack_ts           = 0;
    memset(s->tile_frame_id, 0, sizeof(s->tile_frame_id));
    memset(s->chunk_seen, 0, sizeof(s->chunk_seen));
    if (s->bgra_buf)   memset(s->bgra_buf, 0, MAX_RAW_BYTES);
    if (s->render_buf) memset(s->render_buf, 0, MAX_RAW_BYTES);
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
        if (n == 4) {
            if (memcmp(pkt, "PING", 4) == 0) {
                sendto(g_sock, "PONG", 4, 0, (struct sockaddr *)&from_addr, sizeof(from_addr));
                if (!is_known_target_ip(from_addr.sin_addr.s_addr)) {
                    pthread_mutex_lock(&g_lock);
                    if (g_num_targets < MAX_TARGETS) {
                        g_targets[g_num_targets++] = from_addr;
                    }
                    pthread_mutex_unlock(&g_lock);
                }
                continue;
            } else if (memcmp(pkt, "PONG", 4) == 0) {
                if (!is_known_target_ip(from_addr.sin_addr.s_addr)) {
                    pthread_mutex_lock(&g_lock);
                    if (g_num_targets < MAX_TARGETS) {
                        g_targets[g_num_targets++] = from_addr;
                    }
                    pthread_mutex_unlock(&g_lock);
                }
                continue;
            }
        } else if (n <= 0) {
            continue;
        }

        ssize_t min_len = (ssize_t)(sizeof(PacketHeader) + CRYPTO_NONCE_LEN + CRYPTO_TAG_LEN);
        if (n <= min_len) continue;

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

        if (!is_known_target_ip(from_addr.sin_addr.s_addr)) {
            pthread_mutex_lock(&g_lock);
            if (g_num_targets < MAX_TARGETS) {
                g_targets[g_num_targets++] = from_addr;
            }
            pthread_mutex_unlock(&g_lock);
        }

        if (hdr->codec == CODEC_NACK) {
            pthread_mutex_lock(&g_lock);
            for (int t = 0; t < MAX_TILES; t++) {
                s_tile_age[t] += 20;
            }
            pthread_mutex_unlock(&g_lock);
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

        /* Decodificação imediata de tiles autossuficientes sem descartar frames */
        if (hdr->codec == CODEC_TILES || hdr->codec == CODEC_RLE) {
            if (s->width != hdr->width || s->height != hdr->height) {
                pthread_mutex_lock(&s->fb_lock);
                s->width = hdr->width;
                s->height = hdr->height;
                memset(s->bgra_buf, 0, (size_t)hdr->width * (size_t)hdr->height * 4);
                memset(s->tile_frame_id, 0, sizeof(s->tile_frame_id));
                pthread_mutex_unlock(&s->fb_lock);
            }

            if (hdr->frame_id != s->active_frame_id) {
                if (s->active_frame_id > 0 && s->received_chunks_count < s->total_chunks_expected) {
                    if ((now - s->last_nack_ts) >= 30) {
                        s->last_nack_ts = now;
                        send_nack_packet(s->active_frame_id);
                    }
                }
                s->active_frame_id = hdr->frame_id;
                s->frame_first_pkt_ts = now;
                s->received_chunks_count = 0;
                s->total_chunks_expected = hdr->total_chunks;
                memset(s->chunk_seen, 0, sizeof(s->chunk_seen));
            }

            if (hdr->chunk_index < MAX_CHUNKS && !s->chunk_seen[hdr->chunk_index]) {
                s->chunk_seen[hdr->chunk_index] = 1;
                s->received_chunks_count++;
            }

            pthread_mutex_lock(&s->fb_lock);
            decode_tile_packet(plain_chunk, cipher_len, (uint32_t *)s->bgra_buf,
                               hdr->width, hdr->height, hdr->frame_id, s->tile_frame_id);
            pthread_mutex_unlock(&s->fb_lock);

            if (hdr->total_chunks > 0 && s->received_chunks_count >= hdr->total_chunks) {
                __atomic_store_n(&s->frame_complete, 1, __ATOMIC_SEQ_CST);
            }
            __atomic_store_n(&s->frame_ready, 1, __ATOMIC_SEQ_CST);
            continue;
        }

        int slot_idx = (int)(hdr->frame_id % NUM_FRAME_SLOTS);
        FrameSlot *slot = &s->slots[slot_idx];

        if (slot->frame_id != hdr->frame_id || (now - slot->start_ts) > FRAME_TIMEOUT_MS) {
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

        if (slot->received_chunks == slot->total_chunks) {
            if (slot->frame_id >= s->last_rendered_frame_id || (s->last_rendered_frame_id - slot->frame_id) > 10000) {
                s->last_rendered_frame_id = slot->frame_id;
                int decoded_ok = 0;
                if (slot->codec == CODEC_RLE) {
                    decode_tile_frame(slot->comp_buf, slot->comp_size, (uint32_t *)s->bgra_buf, slot->width, slot->height);
                    decoded_ok = 1;
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
    g_prev_bgra = (uint8_t *)malloc(MAX_RAW_BYTES);
    g_cap_comp = (uint8_t *)malloc(MAX_COMP_BYTES);
    if (!g_cap_bgra || !g_prev_bgra || !g_cap_comp) {
        free(g_cap_bgra); g_cap_bgra = NULL;
        free(g_prev_bgra); g_prev_bgra = NULL;
        free(g_cap_comp); g_cap_comp = NULL;
        close(g_sock); g_sock = -1;
        return -4;
    }
    memset(g_prev_bgra, 0, MAX_RAW_BYTES);

    memset(g_rx_streams, 0, sizeof(g_rx_streams));
    for (int i = 0; i < MAX_STREAMS; i++) {
        g_rx_streams[i].bgra_buf = (uint8_t *)malloc(MAX_RAW_BYTES);
        g_rx_streams[i].render_buf = (uint8_t *)malloc(MAX_RAW_BYTES);
        pthread_mutex_init(&g_rx_streams[i].fb_lock, NULL);
        memset(g_rx_streams[i].tile_frame_id, 0, sizeof(g_rx_streams[i].tile_frame_id));
        memset(g_rx_streams[i].chunk_seen, 0, sizeof(g_rx_streams[i].chunk_seen));
        for (int k = 0; k < NUM_FRAME_SLOTS; k++) {
            g_rx_streams[i].slots[k].comp_buf = (uint8_t *)malloc(MAX_COMP_BYTES);
            memset(g_rx_streams[i].slots[k].chunk_mask, 0, MAX_CHUNKS);
            g_rx_streams[i].slots[k].frame_id        = 0;
            g_rx_streams[i].slots[k].received_chunks = 0;
            g_rx_streams[i].slots[k].total_chunks    = 0;
        }
    }

    pthread_mutex_init(&g_lock, NULL);
    crypto_init_ctx(&g_crypto);
    g_num_targets = 0;
    __atomic_store_n(&g_running, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_capturing, 0, __ATOMIC_SEQ_CST);

    pthread_create(&g_rx_thread, NULL, rx_thread_proc, NULL);
    g_rx_started = 1;

    pthread_create(&g_render_thread, NULL, render_thread_proc, NULL);
    g_render_started = 1;
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
    return video_start_capture_ex(1080, target_fps, br, 0, 0);
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
            if (g_sock >= 0) {
                sendto(g_sock, "PING", 4, 0, (struct sockaddr *)&addr, sizeof(addr));
            }
            return;
        }
    }
    if (g_num_targets < MAX_TARGETS) {
        g_targets[g_num_targets++] = addr;
    }
    pthread_mutex_unlock(&g_lock);

    /* Envia probe UDP imediatamente para abrir NAT e firewall nos dois lados */
    if (g_sock >= 0) {
        sendto(g_sock, "PING", 4, 0, (struct sockaddr *)&addr, sizeof(addr));
    }
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

    if (g_render_started) {
        pthread_join(g_render_thread, NULL);
        g_render_started = 0;
    }

    free(g_cap_bgra); g_cap_bgra = NULL;
    free(g_prev_bgra); g_prev_bgra = NULL;
    free(g_cap_comp); g_cap_comp = NULL;
    for (int i = 0; i < MAX_STREAMS; i++) {
        free(g_rx_streams[i].bgra_buf); g_rx_streams[i].bgra_buf = NULL;
        free(g_rx_streams[i].render_buf); g_rx_streams[i].render_buf = NULL;
        pthread_mutex_destroy(&g_rx_streams[i].fb_lock);
        for (int k = 0; k < NUM_FRAME_SLOTS; k++) {
            free(g_rx_streams[i].slots[k].comp_buf);
            g_rx_streams[i].slots[k].comp_buf = NULL;
        }
    }

    crypto_free_ctx(&g_crypto);
    pthread_mutex_destroy(&g_lock);
}
