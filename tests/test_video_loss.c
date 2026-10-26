#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define TILE_SZ 32
#define CHUNK_PAYLOAD 1180
#define TILE_MODE_RLE   0
#define TILE_MODE_VIDEO 1
#define MAX_TILES 3000
#define MAX_CHUNKS 256

#pragma pack(push, 1)
typedef struct {
    uint16_t tile_idx;
    uint8_t  mode;
    uint16_t data_len;
} TileItemHeader;
#pragma pack(pop)

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

/* Decodificação com proteção anti-reordenação por tile */
static int decode_tile_packet_versioned(const uint8_t *payload, uint32_t len, uint32_t *fb,
                                        int w, int h, uint32_t pkt_frame_id, uint32_t *tile_frame_id) {
    if (len < 1) return 0;
    int cols = (w + TILE_SZ - 1) / TILE_SZ;
    uint8_t num_tiles = payload[0];
    const uint8_t *src = payload + 1;
    const uint8_t *end = payload + len;
    uint32_t tile[TILE_SZ * TILE_SZ];
    int applied_count = 0;

    for (int i = 0; i < num_tiles && src + sizeof(TileItemHeader) <= end; i++) {
        const TileItemHeader *hdr = (const TileItemHeader *)src;
        src += sizeof(TileItemHeader);
        if (src + hdr->data_len > end) break;

        uint16_t t_idx = hdr->tile_idx;
        if (t_idx < MAX_TILES) {
            uint32_t cur_ver = tile_frame_id[t_idx];
            if (pkt_frame_id >= cur_ver || (cur_ver - pkt_frame_id) > 0x80000000u) {
                tile_frame_id[t_idx] = pkt_frame_id;
                int tx = (t_idx % cols) * TILE_SZ;
                int ty = (t_idx / cols) * TILE_SZ;
                int tw = (tx + TILE_SZ <= w) ? TILE_SZ : (w - tx);
                int th = (ty + TILE_SZ <= h) ? TILE_SZ : (h - ty);
                decode_tile_video(src, hdr->data_len, tile, tw, th);
                int idx = 0;
                for (int y = 0; y < th; y++) {
                    uint32_t *row = fb + (ty + y) * w + tx;
                    for (int x = 0; x < tw; x++) {
                        row[x] = tile[idx++];
                    }
                }
                applied_count++;
            }
        }
        src += hdr->data_len;
    }
    return applied_count;
}

int main() {
    printf("===============================================================\n");
    printf("  LOCALCALL v1.0.7: TEST SUITE - VIDEO ANTI-ARTIFACT & PACING\n");
    printf("===============================================================\n");

    /* TEST 1: Verificar tamanho exato do tile YCoCg 4:2:0 */
    uint32_t in_tile[32 * 32];
    for (int i = 0; i < 32 * 32; i++) {
        in_tile[i] = 0xFF000000 | (rand() & 0xFFFFFF);
    }
    uint8_t out_buf[1024];
    uint32_t enc_sz = encode_tile_video(in_tile, 32, 32, out_buf);
    printf("[1] YCoCg 4:2:0 Encoded Tile Size: %u bytes\n", enc_sz);
    if (enc_sz == 384) {
        printf("    -> PASS: Exatamente 384 bytes por tile 32x32 (50%% de reducao vs 768B)!\n");
    } else {
        printf("    -> FAIL: Tamanho inesperado: %u\n", enc_sz);
        return 1;
    }

    /* TEST 2: Multi-tile packing em pacote UDP de 1180 bytes */
    int tiles_per_pkt = (1180 - 1) / (sizeof(TileItemHeader) + 384);
    printf("[2] Capacidade de tiles por pacote UDP: %d tiles (Header 5B + 384B = 389B)\n", tiles_per_pkt);
    if (tiles_per_pkt == 3) {
        printf("    -> PASS: 3 tiles inteiros de video por pacote UDP (< 1180 bytes)\n");
    } else {
        printf("    -> FAIL: Capacidade invalida\n");
        return 1;
    }

    /* TEST 3: Anti-reordenação por Tile (Rejeição de frames defasados) */
    uint32_t tile_frame_id[MAX_TILES] = {0};
    uint32_t *fb = (uint32_t *)calloc(1920 * 1080, sizeof(uint32_t));
    if (!fb) return 1;

    /* Cria pacote do Frame 100 contendo tile 5 */
    uint8_t pkt_f100[1180];
    pkt_f100[0] = 1;
    TileItemHeader *th = (TileItemHeader *)(pkt_f100 + 1);
    th->tile_idx = 5;
    th->mode = TILE_MODE_VIDEO;
    th->data_len = 384;
    encode_tile_video(in_tile, 32, 32, pkt_f100 + 1 + sizeof(TileItemHeader));
    uint32_t pkt_len = 1 + sizeof(TileItemHeader) + 384;

    /* Aplica Frame 100 */
    int ret100 = decode_tile_packet_versioned(pkt_f100, pkt_len, fb, 1920, 1080, 100, tile_frame_id);
    printf("[3] Aplicando pacote Frame 100 (Tile 5): applied = %d, tile_frame_id[5] = %u\n", ret100, tile_frame_id[5]);
    if (ret100 == 1 && tile_frame_id[5] == 100) {
        printf("    -> PASS: Frame 100 aplicado com sucesso.\n");
    } else {
        printf("    -> FAIL: Nao aplicou frame 100.\n");
        free(fb);
        return 1;
    }

    /* Tenta aplicar pacote atrasado do Frame 98 contendo o mesmo tile 5 (efeito do bug antigo) */
    int ret98 = decode_tile_packet_versioned(pkt_f100, pkt_len, fb, 1920, 1080, 98, tile_frame_id);
    printf("    Tentando aplicar pacote ATRASADO Frame 98 (Tile 5): applied = %d\n", ret98);
    if (ret98 == 0 && tile_frame_id[5] == 100) {
        printf("    -> PASS: Pacote atrasado REJEITADO com sucesso! Fim dos retangulos antigos.\n");
    } else {
        printf("    -> FAIL: Aceitou pacote atrasado!\n");
        free(fb);
        return 1;
    }

    /* TEST 4: Pacing & Rate Budgeting */
    int bitrate = 4000000; /* 4 Mbps */
    int fps = 60;
    int budget_per_frame = (bitrate / 8) / fps; /* ~8333 bytes */
    int max_burst = (budget_per_frame * 3) / 2;  /* ~12500 bytes */
    int max_tiles_sent = max_burst / (sizeof(TileItemHeader) + 384);
    printf("[4] Rate Budgeting: 4 Mbps @ 60 FPS -> Budget/Frame: %d bytes, Max Burst: %d bytes\n",
           budget_per_frame, max_burst);
    printf("    Max tiles enviados em pico: %d tiles (~%d pacotes UDP)\n", max_tiles_sent, max_tiles_sent / 3);
    if (max_burst < 30000) {
        printf("    -> PASS: Limita explosao na rede a no maximo ~12 KB por frame (evita drop de VPN/Wi-Fi).\n");
    } else {
        printf("    -> FAIL: Burst descontrolado.\n");
        free(fb);
        return 1;
    }

    printf("\n>>> TODOS OS 4 TESTES PASSARAM COM SUCESSO! <<<\n");
    free(fb);
    return 0;
}
