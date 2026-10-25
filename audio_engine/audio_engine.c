/*
 * Motor de Áudio VoIP P2P Cifrado + WASAPI Loopback (Áudio do Sistema) + Indicador de Voz (VAD)
 * - Captura de Microfone: WinMM (PCM 16-bit, 24000 Hz, Mono, frames de 20 ms = 480 amostras)
 * - Captura do Sistema: WASAPI Loopback (AUDCLNT_STREAMFLAGS_LOOPBACK) com reamostragem para 24 kHz
 * - Anti-Eco no Loopback: Envia um pacote separado (AUDIO_MAGIC_LOOP = 0xA2) para que os amigos
 *   não escutem a própria voz duplicada quando alguém compartilha o áudio do sistema.
 * - Criptografia: AES-256-GCM via Windows CNG (bcrypt.h)
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "c_crypto.h"

#define EXPORT __declspec(dllexport)

#define SAMPLE_RATE      24000
#define FRAME_SAMPLES    480                        /* 20 ms @ 24 kHz */
#define FRAME_BYTES      (FRAME_SAMPLES * 2)        /* 960 bytes PCM16 */
#define HW_FRAME_SAMPLES 960                        /* 20 ms @ 48 kHz */
#define HW_FRAME_BYTES   (HW_FRAME_SAMPLES * 2)     /* 1920 bytes PCM16 */
#define AUDIO_MAGIC_MIC  0xA1
#define AUDIO_MAGIC_LOOP 0xA2
#define AUDIO_PKT_LEN    (1 + CRYPTO_NONCE_LEN + FRAME_BYTES + CRYPTO_TAG_LEN)
#define AUDIO_PKT_LEN_48K (1 + CRYPTO_NONCE_LEN + HW_FRAME_BYTES + CRYPTO_TAG_LEN)
#define NUM_BUFS         6
#define MAX_PEERS        8
#define MAX_STREAMS      16
#define RING_FRAMES      16
#define VAD_THRESHOLD    220                        /* limiar de energia para Green Ring */
#define VAD_HOLD_MS      280                        /* mantém anel verde aceso por 280ms */

/* GUIDs WASAPI declarados explicitamente para independência de bibliotecas de link */
static const CLSID CLSID_MMDeviceEnumerator_Local =
    {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const IID IID_IMMDeviceEnumerator_Local =
    {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const IID IID_IAudioClient_Local =
    {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const IID IID_IAudioCaptureClient_Local =
    {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_Local =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

#ifndef WAVE_FORMAT_EXTENSIBLE
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#endif

typedef struct {
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
    WORD  wBitsPerSample;
    WORD  cbSize;
    WORD  wValidBitsPerSample;
    DWORD dwChannelMask;
    GUID  SubFormat;
} WAVEFORMATEXTENSIBLE_Local;

typedef struct {
    uint32_t stream_key;                            /* sender_id (mic) ou sender_id | 0x8000 (loopback) */
    uint32_t sender_id;
    DWORD    last_rx;
    DWORD    last_voice_ts;
    int      read_idx;
    int      write_idx;
    int      count;
    int16_t  frames[RING_FRAMES][FRAME_SAMPLES];
} PeerStream;

static CRITICAL_SECTION g_lock;
static CryptoContext    g_crypto;
static volatile LONG    g_running          = 0;
static volatile LONG    g_mic_muted        = 0;
static volatile LONG    g_deafened         = 0;
static volatile LONG    g_loopback_enabled = 0;
static volatile DWORD   g_local_voice_ts   = 0;

static SOCKET             g_sock = INVALID_SOCKET;
static struct sockaddr_in g_peers[MAX_PEERS];
static int                g_num_peers = 0;
static PeerStream         g_streams[MAX_STREAMS];
static int                g_peer_gain_pct[16];      /* Volume individual por sender_id (0% a 150%) */
static int                g_peer_muted[16];         /* Mute local de voz por sender_id */
static int                g_peer_loop_muted[16];    /* Mute local do áudio da tela por sender_id */

static HANDLE g_rx_thread   = NULL;
static HANDLE g_cap_thread  = NULL;
static HANDLE g_play_thread = NULL;
static HANDLE g_loop_thread = NULL;
static HANDLE g_cap_event   = NULL;
static HANDLE g_play_event  = NULL;

static HWAVEIN  g_wave_in  = NULL;
static HWAVEOUT g_wave_out = NULL;
static WAVEHDR  g_in_hdrs[NUM_BUFS];
static WAVEHDR  g_out_hdrs[NUM_BUFS];
static int16_t  g_in_bufs[NUM_BUFS][FRAME_SAMPLES];
static int16_t  g_out_bufs[NUM_BUFS][FRAME_SAMPLES];

/* Calcula média absoluta das amostras PCM para detectar voz ativa (Green Ring) */
static int is_frame_voiced(const int16_t *pcm) {
    int64_t sum = 0;
    for (int i = 0; i < FRAME_SAMPLES; i++) {
        int v = pcm[i];
        sum += (v < 0) ? -v : v;
    }
    return (sum / FRAME_SAMPLES) >= VAD_THRESHOLD;
}

static int is_known_peer_ip(uint32_t ip_addr) {
    int ok = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_num_peers; i++) {
        if (g_peers[i].sin_addr.s_addr == ip_addr) {
            ok = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

static void push_frame(uint32_t stream_key, uint32_t sender_id, const int16_t *pcm, int check_vad) {
    DWORD now = GetTickCount();
    int voiced = check_vad ? is_frame_voiced(pcm) : 0;

    EnterCriticalSection(&g_lock);

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

    LeaveCriticalSection(&g_lock);
}

static void mix_output(int16_t *out_pcm) {
    if (InterlockedCompareExchange(&g_deafened, 0, 0)) {
        memset(out_pcm, 0, FRAME_BYTES);
        return;
    }

    int32_t acc[FRAME_SAMPLES] = {0};
    DWORD now = GetTickCount();

    EnterCriticalSection(&g_lock);
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
    LeaveCriticalSection(&g_lock);

    for (int j = 0; j < FRAME_SAMPLES; j++) {
        int32_t v = acc[j];
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        out_pcm[j] = (int16_t)v;
    }
}

/* Envia um frame PCM cifrado (seja do microfone ou do WASAPI Loopback) para todos os peers */
static void encrypt_and_send_pcm(uint8_t magic, const int16_t *pcm) {
    if (!g_crypto.ready) return;
    uint8_t pkt[AUDIO_PKT_LEN];
    pkt[0] = magic;
    uint8_t *nonce  = pkt + 1;
    uint8_t *cipher = pkt + 1 + CRYPTO_NONCE_LEN;
    uint8_t *tag    = cipher + FRAME_BYTES;

    if (crypto_seal(&g_crypto, pkt, 1, (const uint8_t *)pcm, FRAME_BYTES, nonce, cipher, tag) != 0) {
        return;
    }

    EnterCriticalSection(&g_lock);
    for (int p = 0; p < g_num_peers; p++) {
        sendto(g_sock, (const char *)pkt, AUDIO_PKT_LEN, 0,
               (struct sockaddr *)&g_peers[p], sizeof(struct sockaddr_in));
    }
    LeaveCriticalSection(&g_lock);
}

static DWORD WINAPI rx_thread_proc(LPVOID param) {
    (void)param;
    uint8_t pkt[AUDIO_PKT_LEN_48K + 32];
    int16_t pcm_raw[HW_FRAME_SAMPLES];
    int16_t pcm[FRAME_SAMPLES];
    struct sockaddr_in from_addr;

    while (InterlockedCompareExchange(&g_running, 0, 0)) {
        int from_len = sizeof(from_addr);
        int n = recvfrom(g_sock, (char *)pkt, sizeof(pkt), 0,
                         (struct sockaddr *)&from_addr, &from_len);
        if (n != AUDIO_PKT_LEN && n != AUDIO_PKT_LEN_48K) continue;
        uint8_t magic = pkt[0];
        if (magic != AUDIO_MAGIC_MIC && magic != AUDIO_MAGIC_LOOP) continue;
        if (InterlockedCompareExchange(&g_deafened, 0, 0)) continue;
        if (!is_known_peer_ip(from_addr.sin_addr.s_addr)) continue;

        /* Se estamos enviando nosso próprio loopback, ignoramos loopback alheio para evitar loop infinito */
        if (magic == AUDIO_MAGIC_LOOP && InterlockedCompareExchange(&g_loopback_enabled, 0, 0)) {
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
                for (int i = 0; i < FRAME_SAMPLES; i++) {
                    pcm[i] = (int16_t)(((int32_t)pcm_raw[i * 2] + (int32_t)pcm_raw[i * 2 + 1]) >> 1);
                }
                push_frame(skey, sid, pcm, magic == AUDIO_MAGIC_MIC);
            } else {
                push_frame(skey, sid, pcm_raw, magic == AUDIO_MAGIC_MIC);
            }
        }
    }
    return 0;
}

static DWORD WINAPI cap_thread_proc(LPVOID param) {
    (void)param;
    WAVEFORMATEX fmt = {0};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 1;
    fmt.nSamplesPerSec  = SAMPLE_RATE;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = 2;
    fmt.nAvgBytesPerSec = SAMPLE_RATE * 2;

    if (waveInOpen(&g_wave_in, WAVE_MAPPER, &fmt, (DWORD_PTR)g_cap_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        g_wave_in = NULL;
        return 1;
    }

    for (int i = 0; i < NUM_BUFS; i++) {
        memset(&g_in_hdrs[i], 0, sizeof(WAVEHDR));
        g_in_hdrs[i].lpData         = (LPSTR)g_in_bufs[i];
        g_in_hdrs[i].dwBufferLength = FRAME_BYTES;
        waveInPrepareHeader(g_wave_in, &g_in_hdrs[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_wave_in, &g_in_hdrs[i], sizeof(WAVEHDR));
    }
    waveInStart(g_wave_in);

    while (InterlockedCompareExchange(&g_running, 0, 0)) {
        WaitForSingleObject(g_cap_event, 50);
        for (int i = 0; i < NUM_BUFS; i++) {
            if (g_in_hdrs[i].dwFlags & WHDR_DONE) {
                if (!InterlockedCompareExchange(&g_mic_muted, 0, 0) &&
                    !InterlockedCompareExchange(&g_deafened, 0, 0)) {
                    if (is_frame_voiced(g_in_bufs[i])) {
                        g_local_voice_ts = GetTickCount();
                    }
                    encrypt_and_send_pcm(AUDIO_MAGIC_MIC, g_in_bufs[i]);
                }
                if (InterlockedCompareExchange(&g_running, 0, 0)) {
                    waveInAddBuffer(g_wave_in, &g_in_hdrs[i], sizeof(WAVEHDR));
                }
            }
        }
    }

    waveInReset(g_wave_in);
    for (int i = 0; i < NUM_BUFS; i++) {
        waveInUnprepareHeader(g_wave_in, &g_in_hdrs[i], sizeof(WAVEHDR));
    }
    waveInClose(g_wave_in);
    g_wave_in = NULL;
    return 0;
}

/* Thread WASAPI Loopback: captura o áudio do sistema (jogos/filmes), reamostra para 24 kHz mono e envia */
static DWORD WINAPI loopback_thread_proc(LPVOID param) {
    (void)param;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice           *device     = NULL;
    IAudioClient        *client     = NULL;
    IAudioCaptureClient *capture    = NULL;
    WAVEFORMATEX        *pwfx       = NULL;

    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator_Local, NULL, CLSCTX_ALL,
                                  &IID_IMMDeviceEnumerator_Local, (void **)&enumerator);
    if (FAILED(hr) || !enumerator) { CoUninitialize(); return 1; }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    if (FAILED(hr) || !device) {
        IMMDeviceEnumerator_Release(enumerator);
        CoUninitialize();
        return 1;
    }

    hr = IMMDevice_Activate(device, &IID_IAudioClient_Local, CLSCTX_ALL, NULL, (void **)&client);
    if (FAILED(hr) || !client) {
        IMMDevice_Release(device);
        IMMDeviceEnumerator_Release(enumerator);
        CoUninitialize();
        return 1;
    }

    hr = IAudioClient_GetMixFormat(client, &pwfx);
    if (SUCCEEDED(hr) && pwfx) {
        hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_LOOPBACK,
                                     400000, /* 40ms buffer */
                                     0, pwfx, NULL);
    }

    if (SUCCEEDED(hr)) {
        hr = IAudioClient_GetService(client, &IID_IAudioCaptureClient_Local, (void **)&capture);
    }

    int is_float = 0;
    if (pwfx) {
        if (pwfx->wFormatTag == 3) { /* WAVE_FORMAT_IEEE_FLOAT */
            is_float = 1;
        } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22) {
            WAVEFORMATEXTENSIBLE_Local *ext = (WAVEFORMATEXTENSIBLE_Local *)pwfx;
            if (memcmp(&ext->SubFormat, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT_Local, sizeof(GUID)) == 0) {
                is_float = 1;
            }
        }
    }

    int src_rate = pwfx ? (int)pwfx->nSamplesPerSec : 48000;
    int src_ch   = pwfx ? (int)pwfx->nChannels : 2;
    int started  = 0;

    if (SUCCEEDED(hr) && capture && (is_float || (pwfx && pwfx->wBitsPerSample == 16))) {
        if (SUCCEEDED(IAudioClient_Start(client))) {
            started = 1;
        }
    }

    int16_t out_frame[FRAME_SAMPLES];
    int out_pos = 0;
    double step = (double)src_rate / (double)SAMPLE_RATE;
    double src_cursor = 0.0;

    while (started && InterlockedCompareExchange(&g_running, 0, 0)) {
        if (!InterlockedCompareExchange(&g_loopback_enabled, 0, 0)) {
            out_pos = 0;
            Sleep(25);
            /* Drena pacotes pendentes enquanto desativado */
            UINT32 pkt_len = 0;
            while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(capture, &pkt_len)) && pkt_len > 0) {
                BYTE *data = NULL; UINT32 frames = 0; DWORD flags = 0;
                if (SUCCEEDED(IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL))) {
                    IAudioCaptureClient_ReleaseBuffer(capture, frames);
                } else break;
            }
            continue;
        }

        Sleep(8);
        UINT32 pkt_len = 0;
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(capture, &pkt_len)) && pkt_len > 0) {
            BYTE *data = NULL;
            UINT32 num_frames = 0;
            DWORD flags = 0;
            if (FAILED(IAudioCaptureClient_GetBuffer(capture, &data, &num_frames, &flags, NULL, NULL))) {
                break;
            }

            int silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            while (src_cursor < (double)num_frames) {
                int idx = (int)src_cursor;
                int32_t sample_val = 0;

                if (!silent && data) {
                    if (is_float) {
                        const float *flt = (const float *)data + (idx * src_ch);
                        float mono = (src_ch >= 2) ? ((flt[0] + flt[1]) * 0.5f) : flt[0];
                        /* Atenuação leve (0.70x) para não encobrir a voz dos participantes */
                        sample_val = (int32_t)(mono * 22937.0f);
                    } else {
                        const int16_t *s16 = (const int16_t *)data + (idx * src_ch);
                        int32_t mono = (src_ch >= 2) ? ((s16[0] + s16[1]) / 2) : s16[0];
                        sample_val = (mono * 7) / 10;
                    }
                    if (sample_val > 32767) sample_val = 32767;
                    else if (sample_val < -32768) sample_val = -32768;
                }

                out_frame[out_pos++] = (int16_t)sample_val;
                if (out_pos == FRAME_SAMPLES) {
                    /* Só transmite se não for silêncio total para economizar banda */
                    if (!silent && is_frame_voiced(out_frame)) {
                        encrypt_and_send_pcm(AUDIO_MAGIC_LOOP, out_frame);
                    }
                    out_pos = 0;
                }
                src_cursor += step;
            }
            src_cursor -= (double)num_frames;
            if (src_cursor < 0.0) src_cursor = 0.0;

            IAudioCaptureClient_ReleaseBuffer(capture, num_frames);
        }
    }

    if (started) IAudioClient_Stop(client);
    if (pwfx)    CoTaskMemFree(pwfx);
    if (capture) IAudioCaptureClient_Release(capture);
    if (client)  IAudioClient_Release(client);
    if (device)  IMMDevice_Release(device);
    if (enumerator) IMMDeviceEnumerator_Release(enumerator);
    CoUninitialize();
    return 0;
}

static DWORD WINAPI play_thread_proc(LPVOID param) {
    (void)param;
    WAVEFORMATEX fmt = {0};
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 1;
    fmt.nSamplesPerSec  = SAMPLE_RATE;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = 2;
    fmt.nAvgBytesPerSec = SAMPLE_RATE * 2;

    if (waveOutOpen(&g_wave_out, WAVE_MAPPER, &fmt, (DWORD_PTR)g_play_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        g_wave_out = NULL;
        return 1;
    }

    for (int i = 0; i < NUM_BUFS; i++) {
        memset(&g_out_hdrs[i], 0, sizeof(WAVEHDR));
        memset(g_out_bufs[i], 0, FRAME_BYTES);
        g_out_hdrs[i].lpData         = (LPSTR)g_out_bufs[i];
        g_out_hdrs[i].dwBufferLength = FRAME_BYTES;
        waveOutPrepareHeader(g_wave_out, &g_out_hdrs[i], sizeof(WAVEHDR));
        waveOutWrite(g_wave_out, &g_out_hdrs[i], sizeof(WAVEHDR));
    }

    while (InterlockedCompareExchange(&g_running, 0, 0)) {
        WaitForSingleObject(g_play_event, 20);
        for (int i = 0; i < NUM_BUFS; i++) {
            if ((g_out_hdrs[i].dwFlags & WHDR_DONE) && InterlockedCompareExchange(&g_running, 0, 0)) {
                mix_output(g_out_bufs[i]);
                waveOutWrite(g_wave_out, &g_out_hdrs[i], sizeof(WAVEHDR));
            }
        }
    }

    waveOutReset(g_wave_out);
    for (int i = 0; i < NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_wave_out, &g_out_hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_wave_out);
    g_wave_out = NULL;
    return 0;
}

EXPORT void audio_cleanup(void);

EXPORT int audio_init(int local_udp_port) {
    if (InterlockedCompareExchange(&g_running, 0, 0)) {
        audio_cleanup();
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) {
        WSACleanup();
        return -2;
    }

    DWORD rcv_timeout = 100;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcv_timeout, sizeof(rcv_timeout));

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port        = htons((uint16_t)local_udp_port);

    if (bind(g_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == SOCKET_ERROR) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        WSACleanup();
        return -3;
    }

    InitializeCriticalSection(&g_lock);
    crypto_init_ctx(&g_crypto);
    memset(g_streams, 0, sizeof(g_streams));
    for (int i = 0; i < 16; i++) {
        g_peer_gain_pct[i]   = 100;
        g_peer_muted[i]      = 0;
        g_peer_loop_muted[i] = 0;
    }
    g_num_peers = 0;
    g_local_voice_ts = 0;
    InterlockedExchange(&g_loopback_enabled, 0);
    InterlockedExchange(&g_running, 1);

    g_cap_event  = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_play_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    g_rx_thread   = CreateThread(NULL, 0, rx_thread_proc,       NULL, 0, NULL);
    g_cap_thread  = CreateThread(NULL, 0, cap_thread_proc,      NULL, 0, NULL);
    g_play_thread = CreateThread(NULL, 0, play_thread_proc,     NULL, 0, NULL);
    g_loop_thread = CreateThread(NULL, 0, loopback_thread_proc, NULL, 0, NULL);

    SetThreadPriority(g_rx_thread,   THREAD_PRIORITY_ABOVE_NORMAL);
    SetThreadPriority(g_cap_thread,  THREAD_PRIORITY_HIGHEST);
    SetThreadPriority(g_play_thread, THREAD_PRIORITY_HIGHEST);
    return 0;
}

EXPORT int audio_set_key(const unsigned char *key32, unsigned int sender_id) {
    if (!InterlockedCompareExchange(&g_running, 0, 0) || !key32) return -1;
    return crypto_set_key(&g_crypto, key32, sender_id);
}

EXPORT void audio_set_loopback(int enabled) {
    InterlockedExchange(&g_loopback_enabled, enabled ? 1 : 0);
}

/* Retorna bitmask de sender_ids (1..15) que falaram nos últimos VAD_HOLD_MS ms (para o Green Ring) */
EXPORT unsigned int audio_get_speaking_mask(void) {
    if (!InterlockedCompareExchange(&g_running, 0, 0)) return 0;
    DWORD now = GetTickCount();
    unsigned int mask = 0;

    if (g_crypto.local_sender_id > 0 && g_crypto.local_sender_id < 16) {
        if ((now - g_local_voice_ts) <= VAD_HOLD_MS &&
            !InterlockedCompareExchange(&g_mic_muted, 0, 0) &&
            !InterlockedCompareExchange(&g_deafened, 0, 0)) {
            mask |= (1u << g_crypto.local_sender_id);
        }
    }

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_STREAMS; i++) {
        PeerStream *s = &g_streams[i];
        if (s->sender_id > 0 && s->sender_id < 16 && s->last_voice_ts > 0) {
            if (!g_peer_muted[s->sender_id] && g_peer_gain_pct[s->sender_id] > 0 &&
                (now - s->last_voice_ts) <= VAD_HOLD_MS) {
                mask |= (1u << s->sender_id);
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    return mask;
}

EXPORT void audio_set_peer_volume(unsigned int sender_id, int gain_pct) {
    if (sender_id == 0 || sender_id >= 16) return;
    if (gain_pct < 0) gain_pct = 0;
    else if (gain_pct > 150) gain_pct = 150;
    if (!InterlockedCompareExchange(&g_running, 0, 0)) {
        g_peer_gain_pct[sender_id] = gain_pct;
        return;
    }
    EnterCriticalSection(&g_lock);
    g_peer_gain_pct[sender_id] = gain_pct;
    LeaveCriticalSection(&g_lock);
}

EXPORT void audio_set_peer_muted(unsigned int sender_id, int muted) {
    if (sender_id == 0 || sender_id >= 16) return;
    int val = muted ? 1 : 0;
    if (!InterlockedCompareExchange(&g_running, 0, 0)) {
        g_peer_muted[sender_id] = val;
        return;
    }
    EnterCriticalSection(&g_lock);
    g_peer_muted[sender_id] = val;
    LeaveCriticalSection(&g_lock);
}

EXPORT void audio_set_peer_loopback_muted(unsigned int sender_id, int muted) {
    if (sender_id == 0 || sender_id >= 16) return;
    int val = muted ? 1 : 0;
    if (!InterlockedCompareExchange(&g_running, 0, 0)) {
        g_peer_loop_muted[sender_id] = val;
        return;
    }
    EnterCriticalSection(&g_lock);
    g_peer_loop_muted[sender_id] = val;
    LeaveCriticalSection(&g_lock);
}

EXPORT void audio_add_peer(const char *ip, int remote_port) {
    if (!ip || !InterlockedCompareExchange(&g_running, 0, 0)) return;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)remote_port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) return;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_num_peers; i++) {
        if (g_peers[i].sin_addr.s_addr == addr.sin_addr.s_addr &&
            g_peers[i].sin_port == addr.sin_port) {
            LeaveCriticalSection(&g_lock);
            return;
        }
    }
    if (g_num_peers < MAX_PEERS) {
        g_peers[g_num_peers++] = addr;
    }
    LeaveCriticalSection(&g_lock);
}

EXPORT void audio_remove_peers(void) {
    if (!InterlockedCompareExchange(&g_running, 0, 0)) return;
    EnterCriticalSection(&g_lock);
    g_num_peers = 0;
    memset(g_streams, 0, sizeof(g_streams));
    LeaveCriticalSection(&g_lock);
}

EXPORT void audio_set_mic_muted(int muted) {
    InterlockedExchange(&g_mic_muted, muted ? 1 : 0);
}

EXPORT void audio_set_deafened(int deafened) {
    InterlockedExchange(&g_deafened, deafened ? 1 : 0);
}

EXPORT void audio_cleanup(void) {
    if (!InterlockedCompareExchange(&g_running, 0, 0)) return;
    InterlockedExchange(&g_loopback_enabled, 0);
    InterlockedExchange(&g_running, 0);

    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }

    if (g_cap_event)  SetEvent(g_cap_event);
    if (g_play_event) SetEvent(g_play_event);

    HANDLE threads[4] = {g_rx_thread, g_cap_thread, g_play_thread, g_loop_thread};
    for (int i = 0; i < 4; i++) {
        if (threads[i]) {
            WaitForSingleObject(threads[i], 1200);
            CloseHandle(threads[i]);
        }
    }
    g_rx_thread = g_cap_thread = g_play_thread = g_loop_thread = NULL;

    if (g_cap_event)  { CloseHandle(g_cap_event);  g_cap_event = NULL; }
    if (g_play_event) { CloseHandle(g_play_event); g_play_event = NULL; }

    crypto_free_ctx(&g_crypto);
    DeleteCriticalSection(&g_lock);
    WSACleanup();
}
