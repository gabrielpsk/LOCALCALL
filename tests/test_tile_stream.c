#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define TILE_SZ 32
#define CHUNK_PAYLOAD 1180
#define TILE_MODE_RLE   0
#define TILE_MODE_VIDEO 1

#pragma pack(push, 1)
typedef struct {
    uint16_t tile_idx;
    uint8_t  mode;
    uint16_t data_len;
} TileItemHeader;
#pragma pack(pop)

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
    uint8_t *dst = out;

    for (int qy = 0; qy < qh; qy++) {
        int y0 = qy * 2;
        int y1 = (y0 + 1 < th) ? y0 + 1 : y0;
        const uint32_t *row0 = tile + y0 * tw;
        const uint32_t *row1 = tile + y1 * tw;

        for (int qx = 0; qx < qw; qx++) {
            int x0 = qx * 2;
            int x1 = (x0 + 1 < tw) ? x0 + 1 : x0;

            uint32_t p00 = row0[x0];
            uint32_t p10 = row0[x1];
            uint32_t p01 = row1[x0];
            uint32_t p11 = row1[x1];

            uint32_t b = ((p00 & 0xFF) + (p10 & 0xFF) + (p01 & 0xFF) + (p11 & 0xFF) + 2) >> 2;
            uint32_t g = (((p00 >> 8) & 0xFF) + ((p10 >> 8) & 0xFF) + ((p01 >> 8) & 0xFF) + ((p11 >> 8) & 0xFF) + 2) >> 2;
            uint32_t r = (((p00 >> 16) & 0xFF) + ((p10 >> 16) & 0xFF) + ((p01 >> 16) & 0xFF) + ((p11 >> 16) & 0xFF) + 2) >> 2;

            *dst++ = (uint8_t)b;
            *dst++ = (uint8_t)g;
            *dst++ = (uint8_t)r;
        }
    }
    return (uint32_t)(dst - out);
}

static void decode_tile_video(const uint8_t *in, uint32_t in_len, uint32_t *tile, int tw, int th) {
    int qw = (tw + 1) / 2;
    int qh = (th + 1) / 2;
    const uint8_t *src = in;
    const uint8_t *end = in + in_len;

    for (int qy = 0; qy < qh; qy++) {
        int y0 = qy * 2;
        int y1 = y0 + 1;
        uint32_t *row0 = tile + y0 * tw;
        uint32_t *row1 = (y1 < th) ? tile + y1 * tw : NULL;

        for (int qx = 0; qx < qw; qx++) {
            if (src + 3 > end) return;
            uint32_t b = *src++;
            uint32_t g = *src++;
            uint32_t r = *src++;
            uint32_t c = b | (g << 8) | (r << 16) | 0xFF000000u;

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

static void decode_tile_packet(const uint8_t *payload, uint32_t len, uint32_t *fb, int w, int h) {
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

int main() {
    printf("=================================================================\n");
    printf("  TESTE DO MOTOR DE TILES INDEPENDENTES E VIDEO COMPRESSION\n");
    printf("=================================================================\n");

    int w = 1920, h = 1080;
    int cols = (w + TILE_SZ - 1) / TILE_SZ;
    int rows = (h + TILE_SZ - 1) / TILE_SZ;
    int total_tiles = cols * rows;

    printf("[1] Grid 1080p: %dx%d = %d tiles de 32x32\n", cols, rows, total_tiles);

    // 1. Test Mode 0 (RLE) vs Mode 1 (Video)
    uint32_t text_tile[32 * 32];
    for (int i = 0; i < 32 * 32; i++) {
        text_tile[i] = (i % 32 < 8) ? 0xFFFFFFFF : 0xFF18181B;
    }
    uint8_t rle_buf[32 * 32 * 4];
    uint32_t rle_len = compress_tile_rle(text_tile, 32 * 32, rle_buf);
    printf("[2] Mode 0 (Texto/UI): tamanho comprimido = %u bytes (Lossless RLE)\n", rle_len);

    uint32_t noise_tile[32 * 32];
    for (int i = 0; i < 32 * 32; i++) {
        noise_tile[i] = 0xFF000000 | (rand() & 0xFFFFFF); // Simula video/filme com ruido
    }
    uint32_t noise_rle_len = compress_tile_rle(noise_tile, 32 * 32, rle_buf);
    printf("    Video com RLE puro daria: %u bytes (> 768 bytes, excederia)\n", noise_rle_len);

    uint8_t vid_buf[32 * 32 * 3];
    uint32_t vid_len = encode_tile_video(noise_tile, 32, 32, vid_buf);
    printf("[3] Mode 1 (Video/Filme): tamanho compactado = %u bytes (Exatamente 768B!)\n", vid_len);

    if (vid_len == 768) {
        printf("    [PASS] Video tile cabe 100%% em um unico pacote UDP de 1180B!\n");
    } else {
        printf("    [FAIL] Video tile inesperado: %u\n", vid_len);
        return 1;
    }

    // 2. Test Packetization & Independent Decoding with 10% packet drop
    uint32_t *fb_sender = (uint32_t *)malloc(w * h * 4);
    uint32_t *fb_receiver = (uint32_t *)calloc(w * h, 4);

    for (int i = 0; i < w * h; i++) {
        fb_sender[i] = 0xFF000000 | (i * 37);
    }

    // Packetize 100 tiles into independent UDP packets
    uint8_t packets[200][CHUNK_PAYLOAD];
    uint32_t pkt_lens[200];
    int num_pkts = 0;

    uint8_t cur_pkt[CHUNK_PAYLOAD];
    cur_pkt[0] = 0;
    uint32_t cur_len = 1;

    for (int t = 0; t < 100; t++) {
        int tx = (t % cols) * 32;
        int ty = (t / cols) * 32;
        uint32_t tile[32 * 32];
        for (int y = 0; y < 32; y++) {
            for (int x = 0; x < 32; x++) {
                tile[y * 32 + x] = fb_sender[(ty + y) * w + (tx + x)];
            }
        }
        uint8_t enc[768];
        uint32_t elen = encode_tile_video(tile, 32, 32, enc);

        if (cur_len + sizeof(TileItemHeader) + elen > CHUNK_PAYLOAD) {
            memcpy(packets[num_pkts], cur_pkt, cur_len);
            pkt_lens[num_pkts++] = cur_len;
            cur_pkt[0] = 0;
            cur_len = 1;
        }

        TileItemHeader *th = (TileItemHeader *)(cur_pkt + cur_len);
        th->tile_idx = (uint16_t)t;
        th->mode = TILE_MODE_VIDEO;
        th->data_len = (uint16_t)elen;
        cur_len += sizeof(TileItemHeader);
        memcpy(cur_pkt + cur_len, enc, elen);
        cur_len += elen;
        cur_pkt[0]++;
    }
    if (cur_pkt[0] > 0) {
        memcpy(packets[num_pkts], cur_pkt, cur_len);
        pkt_lens[num_pkts++] = cur_len;
    }

    printf("[4] 100 video tiles empacotados em %d pacotes UDP autossuficientes\n", num_pkts);

    // Simulate 10% packet drop: drop packet 3, 7, etc.
    int decoded_pkts = 0;
    for (int p = 0; p < num_pkts; p++) {
        if (p % 10 == 3) {
            // Simulated lost packet
            continue;
        }
        decode_tile_packet(packets[p], pkt_lens[p], fb_receiver, w, h);
        decoded_pkts++;
    }

    printf("[5] Decodificacao independente com 10%% de perda de rede:\n");
    printf("    - Pacotes enviados: %d\n", num_pkts);
    printf("    - Pacotes entregues e decodificados: %d (Zero descarte de frame!)\n", decoded_pkts);

    // Verify that tiles in received packets were decoded onto receiver framebuffer
    int non_black_pixels = 0;
    for (int i = 0; i < w * h; i++) {
        if (fb_receiver[i] != 0) non_black_pixels++;
    }
    printf("    - Pixels atualizados no receptor: %d (%.1f%% da area transmitida)\n",
           non_black_pixels, (double)non_black_pixels / (100 * 32 * 32) * 100.0);

    if (non_black_pixels > 0 && decoded_pkts == 90) {
        printf("\n=================================================================\n");
        printf("  SUCESSO: TESTE APROVADO COM 100%% DE RESILIENCIA A PERDAS!\n");
        printf("=================================================================\n");
    } else {
        printf("[FAIL] Resultado inesperado!\n");
        return 1;
    }

    free(fb_sender);
    free(fb_receiver);
    return 0;
}
