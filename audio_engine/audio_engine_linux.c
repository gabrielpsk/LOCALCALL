/*
 * Motor de Áudio VoIP P2P Cifrado para Linux (Debian / Ubuntu / Kali Linux)
 * - Captura e Playback: PulseAudio Simple (libpulse-simple) com fallback automático para ALSA (libasound)
 * - Hardware: PCM 16-bit signed LE, 48000 Hz, Mono, buffers de 20 ms (960 amostras)
 * - Rede UDP (Interoperabilidade Total Windows <-> Linux na porta 45001):
 *   * Pacotes UDP idênticos ao audio_engine.c do Windows (AUDIO_MAGIC_MIC = 0xA1, AUDIO_MAGIC_LOOP = 0xA2)
 *   * Payload de rede de 20 ms (480 amostras PCM16 = 960 bytes, abaixo do MTU de VPNs ZeroTier/Radmin),
 *     com suporte transparente na recepção a pacotes de 24 kHz (960B) e 48 kHz (1920B).
 * - Loopback do Sistema: Captura de "@DEFAULT_MONITOR@" via PulseAudio com anti-eco (0xA2)
 * - Criptografia: AES-256-GCM via OpenSSL nativo (libcrypto - EVP_aes_256_gcm)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pulse/simple.h>
#include <pulse/error.h>
#include <alsa/asoundlib.h>

#include "c_crypto.h"

#define EXPORT __attribute__((visibility("default")))

/* Taxa de hardware no Linux (48 kHz, 20 ms = 960 amostras) */
#define HW_SAMPLE_RATE     48000
#define HW_FRAME_SAMPLES   960
#define HW_FRAME_BYTES     (HW_FRAME_SAMPLES * 2)

/* Formato de pacote na rede idêntico ao Windows (20 ms = 480 amostras = 960 bytes PCM16) */
#define NET_SAMPLE_RATE    24000
#define FRAME_SAMPLES      480
#define FRAME_BYTES        (FRAME_SAMPLES * 2)

#define AUDIO_MAGIC_MIC    0xA1
#define AUDIO_MAGIC_LOOP   0xA2
#define AUDIO_PKT_LEN      (1 + CRYPTO_NONCE_LEN + FRAME_BYTES + CRYPTO_TAG_LEN)
#define AUDIO_PKT_LEN_48K  (1 + CRYPTO_NONCE_LEN + HW_FRAME_BYTES + CRYPTO_TAG_LEN)

#define MAX_PEERS          8
#define MAX_STREAMS        16
#define RING_FRAMES        16
#define VAD_THRESHOLD      220   /* limiar de energia para Green Ring */
#define VAD_HOLD_MS        280   /* mantém anel verde aceso por 280 ms */

typedef struct {
    uint32_t stream_key;         /* sender_id (mic) ou sender_id | 0x8000 (loopback) */
    uint32_t sender_id;
    uint64_t last_rx;
    uint64_t last_voice_ts;
    int      read_idx;
    int      write_idx;
    int      count;
    int16_t  frames[RING_FRAMES][FRAME_SAMPLES];
} PeerStream;

static pthread_mutex_t    g_lock = PTHREAD_MUTEX_INITIALIZER;
static CryptoContext      g_crypto;
static volatile int       g_running          = 0;
static volatile int       g_mic_muted        = 0;
static volatile int       g_deafened         = 0;
static volatile int       g_loopback_enabled = 0;
static volatile int       g_noise_suppression = 1;
static volatile uint64_t  g_local_voice_ts   = 0;

/* Estado do filtro passa-altas de 80 Hz (Biquad Butterworth 2ª ordem) */
static double g_hpf_x1 = 0.0, g_hpf_x2 = 0.0;
static double g_hpf_y1 = 0.0, g_hpf_y2 = 0.0;

static const double HPF_B0 =  0.98529737;
static const double HPF_B1 = -1.97059474;
static const double HPF_B2 =  0.98529737;
static const double HPF_A1 = -1.97037494;
static const double HPF_A2 =  0.97081454;

static double g_noise_floor = 80.0;
static double g_gate_gain   = 0.0;
static int    g_gate_hold   = 0;

static void apply_noise_suppression(int16_t *pcm) {
    if (!__atomic_load_n(&g_noise_suppression, __ATOMIC_SEQ_CST)) {
        return;
    }

    double x1 = g_hpf_x1, x2 = g_hpf_x2;
    double y1 = g_hpf_y1, y2 = g_hpf_y2;

    for (int i = 0; i < FRAME_SAMPLES; i++) {
        double x0 = (double)pcm[i];
        double y0 = HPF_B0 * x0 + HPF_B1 * x1 + HPF_B2 * x2 - HPF_A1 * y1 - HPF_A2 * y2;
        x2 = x1; x1 = x0;
        y2 = y1; y1 = y0;
        pcm[i] = (int16_t)(y0 > 32767.0 ? 32767 : (y0 < -32768.0 ? -32768 : y0));
    }
    g_hpf_x1 = x1; g_hpf_x2 = x2;
    g_hpf_y1 = y1; g_hpf_y2 = y2;

    int64_t energy_sum = 0;
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        int v = pcm[i];
        energy_sum += (v < 0) ? -v : v;
    }
    double frame_energy = (double)energy_sum / (double)FRAME_SAMPLES;

    if (frame_energy < g_noise_floor) {
        g_noise_floor = 0.90 * g_noise_floor + 0.10 * frame_energy;
    } else if (frame_energy < g_noise_floor * 2.5) {
        g_noise_floor = 0.995 * g_noise_floor + 0.005 * frame_energy;
    }
    if (g_noise_floor < 25.0) g_noise_floor = 25.0;
    if (g_noise_floor > 600.0) g_noise_floor = 600.0;

    double thresh_open  = g_noise_floor * 2.2;
    if (thresh_open < 180.0) thresh_open = 180.0;
    double thresh_close = g_noise_floor * 1.3;
    if (thresh_close < 100.0) thresh_close = 100.0;

    if (frame_energy >= thresh_open) {
        g_gate_hold = 14;
    } else if (frame_energy < thresh_close) {
        if (g_gate_hold > 0) g_gate_hold--;
    }

    double target_gain = (g_gate_hold > 0) ? 1.0 : 0.0;
    double gain = g_gate_gain;
    double attack_rate  = 0.35;
    double release_rate = 0.08;

    for (int i = 0; i < FRAME_SAMPLES; i++) {
        if (target_gain > gain) {
            gain += (target_gain - gain) * (attack_rate / (double)FRAME_SAMPLES * 8.0);
            if (gain > target_gain) gain = target_gain;
        } else {
            gain += (target_gain - gain) * (release_rate / (double)FRAME_SAMPLES * 8.0);
            if (gain < target_gain) gain = target_gain;
        }
        int32_t sample = (int32_t)((double)pcm[i] * gain);
        pcm[i] = (int16_t)(sample > 32767 ? 32767 : (sample < -32768 ? -32768 : sample));
    }
    g_gate_gain = gain;
}

static int                g_sock = -1;
static struct sockaddr_in g_peers[MAX_PEERS];
static int                g_num_peers = 0;
static PeerStream         g_streams[MAX_STREAMS];
static int                g_peer_gain_pct[16];      /* Volume individual por sender_id (0% a 150%) */
static int                g_peer_muted[16];         /* Mute local de voz por sender_id */
static int                g_peer_loop_muted[16];    /* Mute local do áudio da tela por sender_id */

static pthread_t          g_rx_thread;
static pthread_t          g_cap_thread;
static pthread_t          g_play_thread;
static pthread_t          g_loop_thread;
static int                g_threads_started = 0;

/* Relógio monotônico em milissegundos (equivalente ao GetTickCount do Windows) */
static uint64_t get_tick_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Calcula média absoluta das amostras PCM para detectar voz ativa (Green Ring) */
static int is_frame_voiced(const int16_t *pcm, int num_samples) {
    int64_t sum = 0;
    for (int i = 0; i < num_samples; i++) {
        int v = pcm[i];
        sum += (v < 0) ? -v : v;
    }
    return (sum / num_samples) >= VAD_THRESHOLD;
}

/* Converte 48 kHz (960 amostras) -> 24 kHz (480 amostras) com filtro passa-baixa 2:1 */
static void downsample_48k_to_24k(const int16_t *in_48k, int16_t *out_24k) {
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        int32_t s0 = in_48k[i * 2];
        int32_t s1 = in_48k[i * 2 + 1];
        out_24k[i] = (int16_t)((s0 + s1) >> 1);
    }
}

/* Converte 24 kHz (480 amostras) -> 48 kHz (960 amostras) com interpolação linear */
static void upsample_24k_to_48k(const int16_t *in_24k, int16_t *out_48k, int16_t *prev_sample) {
    int16_t last = *prev_sample;
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        int16_t cur = in_24k[i];
        out_48k[i * 2]     = (int16_t)(((int32_t)last + (int32_t)cur) >> 1);
        out_48k[i * 2 + 1] = cur;
        last = cur;
    }
    *prev_sample = last;
}

static int is_known_peer_ip(uint32_t ip_addr) {
    int ok = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_num_peers; i++) {
        if (g_peers[i].sin_addr.s_addr == ip_addr) {
            ok = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static void push_frame(uint32_t stream_key, uint32_t sender_id, const int16_t *pcm, int check_vad) {
    uint64_t now = get_tick_ms();
    int voiced = check_vad ? is_frame_voiced(pcm, FRAME_SAMPLES) : 0;

    pthread_mutex_lock(&g_lock);

    PeerStream *target = NULL;
    PeerStream *oldest = &g_streams[0];

    for (int i = 0; i < MAX_STREAMS; i++) {
        if (g_streams[i].stream_key == stream_key) {
            target = &g_streams[i];
            break;
        }
        if (g_streams[i].last_rx < oldest->last_rx) {
            oldest = &g_streams[i];
        }
    }
    if (!target) {
        target = oldest;
        target->stream_key    = stream_key;
        target->sender_id     = sender_id;
        target->read_idx      = 0;
        target->write_idx     = 0;
        target->count         = 0;
        target->last_voice_ts = 0;
    }

    memcpy(target->frames[target->write_idx], pcm, FRAME_BYTES);
    target->write_idx = (target->write_idx + 1) % RING_FRAMES;
    if (target->count < RING_FRAMES) {
        target->count++;
    } else {
        target->read_idx = (target->read_idx + 1) % RING_FRAMES;
    }
    target->last_rx = now;
    if (voiced) {
        target->last_voice_ts = now;
    }

    pthread_mutex_unlock(&g_lock);
}

static void mix_output(int16_t *out_pcm) {
    if (__atomic_load_n(&g_deafened, __ATOMIC_SEQ_CST)) {
        memset(out_pcm, 0, FRAME_BYTES);
        return;
    }

    int32_t acc[FRAME_SAMPLES] = {0};
    uint64_t now = get_tick_ms();

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_STREAMS; i++) {
        PeerStream *s = &g_streams[i];
        if (s->stream_key != 0 && s->count > 0 && (now - s->last_rx) < 1000) {
            uint32_t sid = s->sender_id & 0x0Fu;
            int is_loop = (s->stream_key & 0x8000u) != 0;
            int muted = is_loop ? g_peer_loop_muted[sid] : g_peer_muted[sid];
            int gain  = g_peer_gain_pct[sid];

            if (!muted && gain > 0) {
                const int16_t *src = s->frames[s->read_idx];
                if (gain == 100) {
                    for (int j = 0; j < FRAME_SAMPLES; j++) {
                        acc[j] += src[j];
                    }
                } else {
                    for (int j = 0; j < FRAME_SAMPLES; j++) {
                        acc[j] += ((int32_t)src[j] * gain) / 100;
                    }
                }
            }
            s->read_idx = (s->read_idx + 1) % RING_FRAMES;
            s->count--;
        }
    }
    pthread_mutex_unlock(&g_lock);

    for (int j = 0; j < FRAME_SAMPLES; j++) {
        int32_t v = acc[j];
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        out_pcm[j] = (int16_t)v;
    }
}

/* Envia um frame PCM cifrado (AES-256-GCM) idêntico ao protocolo Windows */
static void encrypt_and_send_pcm(uint8_t magic, const int16_t *pcm) {
    if (!g_crypto.ready || g_sock < 0) return;
    uint8_t pkt[AUDIO_PKT_LEN];
    pkt[0] = magic;
    uint8_t *nonce  = pkt + 1;
    uint8_t *cipher = pkt + 1 + CRYPTO_NONCE_LEN;
    uint8_t *tag    = cipher + FRAME_BYTES;

    if (crypto_seal(&g_crypto, pkt, 1, (const uint8_t *)pcm, FRAME_BYTES, nonce, cipher, tag) != 0) {
        return;
    }

    pthread_mutex_lock(&g_lock);
    for (int p = 0; p < g_num_peers; p++) {
        sendto(g_sock, pkt, AUDIO_PKT_LEN, MSG_NOSIGNAL,
               (struct sockaddr *)&g_peers[p], sizeof(struct sockaddr_in));
    }
    pthread_mutex_unlock(&g_lock);
}

/* Thread de recepção UDP: decifra pacotes vindos de Windows ou Linux */
static void *rx_thread_proc(void *param) {
    (void)param;
    uint8_t pkt[AUDIO_PKT_LEN_48K + 32];
    int16_t pcm_raw[HW_FRAME_SAMPLES];
    int16_t pcm_24k[FRAME_SAMPLES];
    struct sockaddr_in from_addr;

    while (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(g_sock, pkt, sizeof(pkt), 0,
                             (struct sockaddr *)&from_addr, &from_len);
        if (n != AUDIO_PKT_LEN && n != AUDIO_PKT_LEN_48K) continue;

        uint8_t magic = pkt[0];
        if (magic != AUDIO_MAGIC_MIC && magic != AUDIO_MAGIC_LOOP) continue;
        if (__atomic_load_n(&g_deafened, __ATOMIC_SEQ_CST)) continue;
        if (!is_known_peer_ip(from_addr.sin_addr.s_addr)) continue;

        /* Evita eco infinito se estivermos transmitindo nosso próprio loopback */
        if (magic == AUDIO_MAGIC_LOOP && __atomic_load_n(&g_loopback_enabled, __ATOMIC_SEQ_CST)) {
            continue;
        }

        uint32_t payload_bytes = (n == AUDIO_PKT_LEN) ? FRAME_BYTES : HW_FRAME_BYTES;
        uint8_t *nonce  = pkt + 1;
        uint8_t *cipher = pkt + 1 + CRYPTO_NONCE_LEN;
        uint8_t *tag    = cipher + payload_bytes;
        uint32_t sid    = 0;

        if (crypto_open(&g_crypto, pkt, 1, nonce, cipher, payload_bytes, tag,
                        (uint8_t *)pcm_raw, &sid) == 0 && sid != 0 && sid < 16) {
            uint32_t skey = (magic == AUDIO_MAGIC_LOOP) ? (sid | 0x8000u) : sid;
            if (payload_bytes == HW_FRAME_BYTES) {
                downsample_48k_to_24k(pcm_raw, pcm_24k);
                push_frame(skey, sid, pcm_24k, magic == AUDIO_MAGIC_MIC);
            } else {
                push_frame(skey, sid, pcm_raw, magic == AUDIO_MAGIC_MIC);
            }
        }
    }
    return NULL;
}

/* Thread de captura de microfone: PulseAudio Simple (primário) + ALSA (fallback) */
static void *cap_thread_proc(void *param) {
    (void)param;
    pa_simple *pa_in = NULL;
    snd_pcm_t *alsa_in = NULL;
    int err = 0;

    pa_sample_spec ss = {
        .format   = PA_SAMPLE_S16LE,
        .rate     = HW_SAMPLE_RATE,
        .channels = 1
    };
    pa_buffer_attr ba = {
        .maxlength = (uint32_t)-1,
        .tlength   = (uint32_t)-1,
        .prebuf    = (uint32_t)-1,
        .minreq    = (uint32_t)-1,
        .fragsize  = HW_FRAME_BYTES
    };

    pa_in = pa_simple_new(NULL, "AppDeCall", PA_STREAM_RECORD, NULL,
                          "VoIP Microphone", &ss, NULL, &ba, &err);
    if (!pa_in) {
        if (snd_pcm_open(&alsa_in, "default", SND_PCM_STREAM_CAPTURE, 0) == 0) {
            if (snd_pcm_set_params(alsa_in, SND_PCM_FORMAT_S16_LE,
                                   SND_PCM_ACCESS_RW_INTERLEAVED, 1,
                                   HW_SAMPLE_RATE, 1, 40000) < 0) {
                snd_pcm_close(alsa_in);
                alsa_in = NULL;
            }
        }
    }

    int16_t buf_48k[HW_FRAME_SAMPLES];
    int16_t buf_24k[FRAME_SAMPLES];

    while (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
        int captured_ok = 0;

        if (pa_in) {
            if (pa_simple_read(pa_in, buf_48k, HW_FRAME_BYTES, &err) >= 0) {
                captured_ok = 1;
            } else {
                sleep_ms(10);
            }
        } else if (alsa_in) {
            snd_pcm_sframes_t frames = snd_pcm_readi(alsa_in, buf_48k, HW_FRAME_SAMPLES);
            if (frames < 0) {
                frames = snd_pcm_recover(alsa_in, (int)frames, 1);
            }
            if (frames == HW_FRAME_SAMPLES) {
                captured_ok = 1;
            } else {
                sleep_ms(10);
            }
        } else {
            sleep_ms(20);
        }

        if (captured_ok &&
            !__atomic_load_n(&g_mic_muted, __ATOMIC_SEQ_CST) &&
            !__atomic_load_n(&g_deafened, __ATOMIC_SEQ_CST)) {
            downsample_48k_to_24k(buf_48k, buf_24k);
            apply_noise_suppression(buf_24k);
            if (is_frame_voiced(buf_24k, FRAME_SAMPLES)) {
                __atomic_store_n(&g_local_voice_ts, get_tick_ms(), __ATOMIC_SEQ_CST);
            }
            encrypt_and_send_pcm(AUDIO_MAGIC_MIC, buf_24k);
        }
    }

    if (pa_in)   pa_simple_free(pa_in);
    if (alsa_in) snd_pcm_close(alsa_in);
    return NULL;
}

/* Thread de playback de áudio: PulseAudio Simple (primário) + ALSA (fallback) */
static void *play_thread_proc(void *param) {
    (void)param;
    pa_simple *pa_out = NULL;
    snd_pcm_t *alsa_out = NULL;
    int err = 0;

    pa_sample_spec ss = {
        .format   = PA_SAMPLE_S16LE,
        .rate     = HW_SAMPLE_RATE,
        .channels = 1
    };
    pa_buffer_attr ba = {
        .maxlength = HW_FRAME_BYTES * 4,
        .tlength   = HW_FRAME_BYTES * 2,
        .prebuf    = HW_FRAME_BYTES,
        .minreq    = HW_FRAME_BYTES,
        .fragsize  = (uint32_t)-1
    };

    pa_out = pa_simple_new(NULL, "AppDeCall", PA_STREAM_PLAYBACK, NULL,
                           "VoIP Output", &ss, NULL, &ba, &err);
    if (!pa_out) {
        if (snd_pcm_open(&alsa_out, "default", SND_PCM_STREAM_PLAYBACK, 0) == 0) {
            if (snd_pcm_set_params(alsa_out, SND_PCM_FORMAT_S16_LE,
                                   SND_PCM_ACCESS_RW_INTERLEAVED, 1,
                                   HW_SAMPLE_RATE, 1, 40000) < 0) {
                snd_pcm_close(alsa_out);
                alsa_out = NULL;
            }
        }
    }

    int16_t buf_24k[FRAME_SAMPLES];
    int16_t buf_48k[HW_FRAME_SAMPLES];
    int16_t prev_sample = 0;

    while (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
        mix_output(buf_24k);
        upsample_24k_to_48k(buf_24k, buf_48k, &prev_sample);

        if (pa_out) {
            if (pa_simple_write(pa_out, buf_48k, HW_FRAME_BYTES, &err) < 0) {
                sleep_ms(20);
            }
        } else if (alsa_out) {
            snd_pcm_sframes_t frames = snd_pcm_writei(alsa_out, buf_48k, HW_FRAME_SAMPLES);
            if (frames < 0) {
                snd_pcm_recover(alsa_out, (int)frames, 1);
            }
        } else {
            sleep_ms(20);
        }
    }

    if (pa_out)   pa_simple_free(pa_out);
    if (alsa_out) snd_pcm_close(alsa_out);
    return NULL;
}

/* Thread de Loopback no Linux: captura o Monitor do PulseAudio (@DEFAULT_MONITOR@) quando ativado */
static void *loopback_thread_proc(void *param) {
    (void)param;
    pa_simple *pa_loop = NULL;
    int err = 0;

    pa_sample_spec ss = {
        .format   = PA_SAMPLE_S16LE,
        .rate     = HW_SAMPLE_RATE,
        .channels = 1
    };
    pa_buffer_attr ba = {
        .maxlength = (uint32_t)-1,
        .tlength   = (uint32_t)-1,
        .prebuf    = (uint32_t)-1,
        .minreq    = (uint32_t)-1,
        .fragsize  = HW_FRAME_BYTES
    };

    int16_t buf_48k[HW_FRAME_SAMPLES];
    int16_t buf_24k[FRAME_SAMPLES];

    while (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
        if (!__atomic_load_n(&g_loopback_enabled, __ATOMIC_SEQ_CST)) {
            if (pa_loop) {
                pa_simple_free(pa_loop);
                pa_loop = NULL;
            }
            sleep_ms(30);
            continue;
        }

        if (!pa_loop) {
            pa_loop = pa_simple_new(NULL, "AppDeCall", PA_STREAM_RECORD,
                                    "@DEFAULT_MONITOR@", "System Audio Share",
                                    &ss, NULL, &ba, &err);
            if (!pa_loop) {
                sleep_ms(200);
                continue;
            }
        }

        if (pa_simple_read(pa_loop, buf_48k, HW_FRAME_BYTES, &err) >= 0) {
            downsample_48k_to_24k(buf_48k, buf_24k);
            /* Atenuação leve (0.70x) idêntica ao Windows para não encobrir as vozes */
            for (int i = 0; i < FRAME_SAMPLES; i++) {
                buf_24k[i] = (int16_t)(((int32_t)buf_24k[i] * 7) / 10);
            }
            if (is_frame_voiced(buf_24k, FRAME_SAMPLES)) {
                encrypt_and_send_pcm(AUDIO_MAGIC_LOOP, buf_24k);
            }
        } else {
            pa_simple_free(pa_loop);
            pa_loop = NULL;
            sleep_ms(50);
        }
    }

    if (pa_loop) pa_simple_free(pa_loop);
    return NULL;
}

EXPORT void audio_cleanup(void);

EXPORT int audio_init(int local_udp_port) {
    if (__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
        audio_cleanup();
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock < 0) return -2;

    int reuse = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 100000; /* 100 ms timeout para encerramento limpo da thread RX */
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

    pthread_mutex_init(&g_lock, NULL);
    crypto_init_ctx(&g_crypto);
    memset(g_streams, 0, sizeof(g_streams));
    for (int i = 0; i < 16; i++) {
        g_peer_gain_pct[i]   = 100;
        g_peer_muted[i]      = 0;
        g_peer_loop_muted[i] = 0;
    }
    g_num_peers = 0;
    __atomic_store_n(&g_local_voice_ts, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_loopback_enabled, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_running, 1, __ATOMIC_SEQ_CST);

    pthread_create(&g_rx_thread,   NULL, rx_thread_proc,       NULL);
    pthread_create(&g_cap_thread,  NULL, cap_thread_proc,      NULL);
    pthread_create(&g_play_thread, NULL, play_thread_proc,     NULL);
    pthread_create(&g_loop_thread, NULL, loopback_thread_proc, NULL);
    g_threads_started = 1;

    return 0;
}

EXPORT int audio_set_key(const unsigned char *key32, unsigned int sender_id) {
    if (!__atomic_load_n(&g_running, __ATOMIC_SEQ_CST) || !key32) return -1;
    return crypto_set_key(&g_crypto, key32, sender_id);
}

EXPORT void audio_set_loopback(int enabled) {
    __atomic_store_n(&g_loopback_enabled, enabled ? 1 : 0, __ATOMIC_SEQ_CST);
}

EXPORT unsigned int audio_get_speaking_mask(void) {
    if (!__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) return 0;
    uint64_t now = get_tick_ms();
    unsigned int mask = 0;

    uint64_t local_ts = __atomic_load_n(&g_local_voice_ts, __ATOMIC_SEQ_CST);
    if (g_crypto.local_sender_id > 0 && g_crypto.local_sender_id < 16 && local_ts > 0) {
        if ((now - local_ts) <= VAD_HOLD_MS &&
            !__atomic_load_n(&g_mic_muted, __ATOMIC_SEQ_CST) &&
            !__atomic_load_n(&g_deafened, __ATOMIC_SEQ_CST)) {
            mask |= (1u << g_crypto.local_sender_id);
        }
    }

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_STREAMS; i++) {
        PeerStream *s = &g_streams[i];
        if (s->sender_id > 0 && s->sender_id < 16 && s->last_voice_ts > 0) {
            if (!g_peer_muted[s->sender_id] && g_peer_gain_pct[s->sender_id] > 0 &&
                (now - s->last_voice_ts) <= VAD_HOLD_MS) {
                mask |= (1u << s->sender_id);
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return mask;
}

EXPORT void audio_set_peer_volume(unsigned int sender_id, int gain_pct) {
    if (sender_id == 0 || sender_id >= 16) return;
    if (gain_pct < 0) gain_pct = 0;
    else if (gain_pct > 150) gain_pct = 150;
    if (!__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
        g_peer_gain_pct[sender_id] = gain_pct;
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_peer_gain_pct[sender_id] = gain_pct;
    pthread_mutex_unlock(&g_lock);
}

EXPORT void audio_set_peer_muted(unsigned int sender_id, int muted) {
    if (sender_id == 0 || sender_id >= 16) return;
    int val = muted ? 1 : 0;
    if (!__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
        g_peer_muted[sender_id] = val;
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_peer_muted[sender_id] = val;
    pthread_mutex_unlock(&g_lock);
}

EXPORT void audio_set_peer_loopback_muted(unsigned int sender_id, int muted) {
    if (sender_id == 0 || sender_id >= 16) return;
    int val = muted ? 1 : 0;
    if (!__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) {
        g_peer_loop_muted[sender_id] = val;
        return;
    }
    pthread_mutex_lock(&g_lock);
    g_peer_loop_muted[sender_id] = val;
    pthread_mutex_unlock(&g_lock);
}

EXPORT void audio_add_peer(const char *ip, int remote_port) {
    if (!ip || !__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) return;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)remote_port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_num_peers; i++) {
        if (g_peers[i].sin_addr.s_addr == addr.sin_addr.s_addr &&
            g_peers[i].sin_port == addr.sin_port) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }
    if (g_num_peers < MAX_PEERS) {
        g_peers[g_num_peers++] = addr;
    }
    pthread_mutex_unlock(&g_lock);
}

EXPORT void audio_remove_peers(void) {
    if (!__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) return;
    pthread_mutex_lock(&g_lock);
    g_num_peers = 0;
    memset(g_streams, 0, sizeof(g_streams));
    pthread_mutex_unlock(&g_lock);
}

EXPORT void audio_set_mic_muted(int muted) {
    __atomic_store_n(&g_mic_muted, muted ? 1 : 0, __ATOMIC_SEQ_CST);
}

EXPORT void audio_set_deafened(int deafened) {
    __atomic_store_n(&g_deafened, deafened ? 1 : 0, __ATOMIC_SEQ_CST);
}

EXPORT void audio_set_noise_suppression(int enabled) {
    __atomic_store_n(&g_noise_suppression, enabled ? 1 : 0, __ATOMIC_SEQ_CST);
}

EXPORT int audio_get_noise_suppression(void) {
    return __atomic_load_n(&g_noise_suppression, __ATOMIC_SEQ_CST);
}

EXPORT void audio_cleanup(void) {
    if (!__atomic_load_n(&g_running, __ATOMIC_SEQ_CST)) return;
    __atomic_store_n(&g_loopback_enabled, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_running, 0, __ATOMIC_SEQ_CST);

    if (g_sock >= 0) {
        shutdown(g_sock, SHUT_RDWR);
        close(g_sock);
        g_sock = -1;
    }

    if (g_threads_started) {
        pthread_join(g_rx_thread,   NULL);
        pthread_join(g_cap_thread,  NULL);
        pthread_join(g_play_thread, NULL);
        pthread_join(g_loop_thread, NULL);
        g_threads_started = 0;
    }

    crypto_free_ctx(&g_crypto);
    pthread_mutex_destroy(&g_lock);
}
