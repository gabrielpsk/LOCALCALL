/*
 * Módulo C de Criptografia E2EE Multiplataforma:
 * - Windows: Windows CNG (bcrypt.h)
 * - Linux:   OpenSSL libcrypto (EVP_aes_256_gcm)
 * - Formato idêntico na rede (Windows <-> Linux):
 *   * Chave: 32 bytes (AES-256) derivada via Argon2id + HKDF-SHA256
 *   * Nonce determinístico de 12 bytes: [sender_id:4B big-endian][seq:8B big-endian]
 *   * AAD: [aad_prefix (cabeçalho)][nonce:12B]
 *   * Tag GCM: 16 bytes
 * - Janela deslizante anti-replay de 64 bits por remetente
 */
#ifndef C_CRYPTO_H
#define C_CRYPTO_H

#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#else
#include <pthread.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#endif

#define CRYPTO_KEY_LEN   32
#define CRYPTO_NONCE_LEN 12
#define CRYPTO_TAG_LEN   16
#define MAX_SENDERS      16

typedef struct {
    uint32_t sender_id;
    uint64_t max_seq;
    uint64_t mask;
    int      active;
} ReplaySlot;

typedef struct {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg;
    BCRYPT_KEY_HANDLE hKey;
    UCHAR             key_obj[1024];
    UCHAR             raw_key[CRYPTO_KEY_LEN];
    uint32_t          local_sender_id;
    volatile LONG64   tx_seq;
    int               ready;
    CRITICAL_SECTION  lock;
#else
    EVP_CIPHER_CTX   *enc_ctx;
    EVP_CIPHER_CTX   *dec_ctx;
    uint8_t           raw_key[CRYPTO_KEY_LEN];
    uint32_t          local_sender_id;
    volatile uint64_t tx_seq;
    int               ready;
    pthread_mutex_t   lock;
#endif
    ReplaySlot        replay[MAX_SENDERS];
} CryptoContext;

static inline void write_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static inline void write_be64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)(v);
}

static inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | ((uint32_t)p[3]);
}

static inline uint64_t read_be64(const uint8_t *p) {
    return ((uint64_t)read_be32(p) << 32) | (uint64_t)read_be32(p + 4);
}

/* Comparação em tempo constante */
static inline int ct_memeq(const void *a, const void *b, size_t n) {
    const volatile uint8_t *pa = (const volatile uint8_t *)a;
    const volatile uint8_t *pb = (const volatile uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(pa[i] ^ pb[i]);
    }
    return diff == 0;
}

/* Verifica e atualiza janela deslizante de 64 pacotes por sender_id (chamar com lock adquirido) */
static inline int crypto_replay_check_locked(CryptoContext *ctx, uint32_t sender_id, uint64_t seq, int commit) {
    if (seq == 0) return 0;
    ReplaySlot *slot = NULL;
    ReplaySlot *empty = NULL;

    for (int i = 0; i < MAX_SENDERS; i++) {
        if (ctx->replay[i].active && ctx->replay[i].sender_id == sender_id) {
            slot = &ctx->replay[i];
            break;
        }
        if (!ctx->replay[i].active && !empty) {
            empty = &ctx->replay[i];
        }
    }

    if (!slot) {
        if (!commit) return 1;
        slot = empty ? empty : &ctx->replay[0];
        slot->active    = 1;
        slot->sender_id = sender_id;
        slot->max_seq   = seq;
        slot->mask      = 1ULL;
        return 1;
    }

    if (seq > slot->max_seq) {
        if (commit) {
            uint64_t shift = seq - slot->max_seq;
            if (shift >= 64) slot->mask = 1ULL;
            else slot->mask = (slot->mask << shift) | 1ULL;
            slot->max_seq = seq;
        }
        return 1;
    }

    uint64_t diff = slot->max_seq - seq;
    if (diff >= 64) return 0;
    uint64_t bit = 1ULL << diff;
    if (slot->mask & bit) return 0;
    if (commit) slot->mask |= bit;
    return 1;
}

#ifdef _WIN32

static inline void crypto_init_ctx(CryptoContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    InitializeCriticalSection(&ctx->lock);
}

static inline void crypto_free_ctx(CryptoContext *ctx) {
    EnterCriticalSection(&ctx->lock);
    if (ctx->hKey) {
        BCryptDestroyKey(ctx->hKey);
        ctx->hKey = NULL;
    }
    if (ctx->hAlg) {
        BCryptCloseAlgorithmProvider(ctx->hAlg, 0);
        ctx->hAlg = NULL;
    }
    SecureZeroMemory(ctx->raw_key, sizeof(ctx->raw_key));
    SecureZeroMemory(ctx->key_obj, sizeof(ctx->key_obj));
    SecureZeroMemory(ctx->replay, sizeof(ctx->replay));
    ctx->ready = 0;
    LeaveCriticalSection(&ctx->lock);
    DeleteCriticalSection(&ctx->lock);
}

static inline int crypto_set_key(CryptoContext *ctx, const uint8_t *key32, uint32_t sender_id) {
    if (!key32) return -1;
    EnterCriticalSection(&ctx->lock);

    if (ctx->hKey) { BCryptDestroyKey(ctx->hKey); ctx->hKey = NULL; }
    if (!ctx->hAlg) {
        if (BCryptOpenAlgorithmProvider(&ctx->hAlg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0) {
            LeaveCriticalSection(&ctx->lock);
            return -2;
        }
        if (BCryptSetProperty(ctx->hAlg, BCRYPT_CHAINING_MODE,
                              (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
            BCryptCloseAlgorithmProvider(ctx->hAlg, 0);
            ctx->hAlg = NULL;
            LeaveCriticalSection(&ctx->lock);
            return -3;
        }
    }

    memcpy(ctx->raw_key, key32, CRYPTO_KEY_LEN);
    SecureZeroMemory(ctx->key_obj, sizeof(ctx->key_obj));
    if (BCryptGenerateSymmetricKey(ctx->hAlg, &ctx->hKey,
                                   ctx->key_obj, sizeof(ctx->key_obj),
                                   ctx->raw_key, CRYPTO_KEY_LEN, 0) != 0) {
        SecureZeroMemory(ctx->raw_key, sizeof(ctx->raw_key));
        LeaveCriticalSection(&ctx->lock);
        return -4;
    }

    ctx->local_sender_id = sender_id;
    InterlockedExchange64(&ctx->tx_seq, 0);
    memset(ctx->replay, 0, sizeof(ctx->replay));
    ctx->ready = 1;
    LeaveCriticalSection(&ctx->lock);
    return 0;
}

static inline int crypto_seal(CryptoContext *ctx,
                              const uint8_t *aad_prefix, uint32_t aad_prefix_len,
                              const uint8_t *plain, uint32_t plain_len,
                              uint8_t *nonce_out,
                              uint8_t *cipher_out,
                              uint8_t *tag_out) {
    if (!ctx->ready) return -1;
    uint64_t seq = (uint64_t)InterlockedIncrement64(&ctx->tx_seq);

    write_be32(nonce_out, ctx->local_sender_id);
    write_be64(nonce_out + 4, seq);

    uint8_t aad[64];
    if (aad_prefix_len + CRYPTO_NONCE_LEN > sizeof(aad)) return -2;
    if (aad_prefix_len > 0) memcpy(aad, aad_prefix, aad_prefix_len);
    memcpy(aad + aad_prefix_len, nonce_out, CRYPTO_NONCE_LEN);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce_out;
    info.cbNonce = CRYPTO_NONCE_LEN;
    info.pbAuthData = aad;
    info.cbAuthData = aad_prefix_len + CRYPTO_NONCE_LEN;
    info.pbTag = tag_out;
    info.cbTag = CRYPTO_TAG_LEN;

    ULONG out_len = 0;
    EnterCriticalSection(&ctx->lock);
    NTSTATUS st = BCryptEncrypt(ctx->hKey,
                                (PUCHAR)plain, plain_len,
                                &info, NULL, 0,
                                cipher_out, plain_len, &out_len, 0);
    LeaveCriticalSection(&ctx->lock);
    return (st == 0 && out_len == plain_len) ? 0 : -3;
}

static inline int crypto_open(CryptoContext *ctx,
                              const uint8_t *aad_prefix, uint32_t aad_prefix_len,
                              const uint8_t *nonce,
                              const uint8_t *cipher, uint32_t cipher_len,
                              const uint8_t *tag,
                              uint8_t *plain_out,
                              uint32_t *sender_id_out) {
    if (!ctx->ready) return -1;
    uint32_t sid = read_be32(nonce);
    uint64_t seq = read_be64(nonce + 4);

    uint8_t aad[64];
    if (aad_prefix_len + CRYPTO_NONCE_LEN > sizeof(aad)) return -2;
    if (aad_prefix_len > 0) memcpy(aad, aad_prefix, aad_prefix_len);
    memcpy(aad + aad_prefix_len, nonce, CRYPTO_NONCE_LEN);

    uint8_t local_nonce[CRYPTO_NONCE_LEN];
    uint8_t local_tag[CRYPTO_TAG_LEN];
    memcpy(local_nonce, nonce, CRYPTO_NONCE_LEN);
    memcpy(local_tag, tag, CRYPTO_TAG_LEN);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = local_nonce;
    info.cbNonce = CRYPTO_NONCE_LEN;
    info.pbAuthData = aad;
    info.cbAuthData = aad_prefix_len + CRYPTO_NONCE_LEN;
    info.pbTag = local_tag;
    info.cbTag = CRYPTO_TAG_LEN;

    ULONG out_len = 0;
    EnterCriticalSection(&ctx->lock);
    if (!crypto_replay_check_locked(ctx, sid, seq, 0)) {
        LeaveCriticalSection(&ctx->lock);
        return -3; /* replay detectado */
    }
    NTSTATUS st = BCryptDecrypt(ctx->hKey,
                                (PUCHAR)cipher, cipher_len,
                                &info, NULL, 0,
                                plain_out, cipher_len, &out_len, 0);
    if (st == 0 && out_len == cipher_len) {
        crypto_replay_check_locked(ctx, sid, seq, 1);
        LeaveCriticalSection(&ctx->lock);
        if (sender_id_out) *sender_id_out = sid;
        return 0;
    }
    LeaveCriticalSection(&ctx->lock);
    return -4; /* tag GCM inválida */
}

#else /* Linux / POSIX com OpenSSL (EVP_aes_256_gcm) */

static inline void crypto_init_ctx(CryptoContext *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    pthread_mutex_init(&ctx->lock, NULL);
    ctx->enc_ctx = EVP_CIPHER_CTX_new();
    ctx->dec_ctx = EVP_CIPHER_CTX_new();
}

static inline void crypto_free_ctx(CryptoContext *ctx) {
    pthread_mutex_lock(&ctx->lock);
    if (ctx->enc_ctx) {
        EVP_CIPHER_CTX_free(ctx->enc_ctx);
        ctx->enc_ctx = NULL;
    }
    if (ctx->dec_ctx) {
        EVP_CIPHER_CTX_free(ctx->dec_ctx);
        ctx->dec_ctx = NULL;
    }
    OPENSSL_cleanse(ctx->raw_key, sizeof(ctx->raw_key));
    OPENSSL_cleanse(ctx->replay, sizeof(ctx->replay));
    ctx->ready = 0;
    pthread_mutex_unlock(&ctx->lock);
    pthread_mutex_destroy(&ctx->lock);
}

static inline int crypto_set_key(CryptoContext *ctx, const uint8_t *key32, uint32_t sender_id) {
    if (!key32) return -1;
    pthread_mutex_lock(&ctx->lock);

    if (!ctx->enc_ctx) ctx->enc_ctx = EVP_CIPHER_CTX_new();
    if (!ctx->dec_ctx) ctx->dec_ctx = EVP_CIPHER_CTX_new();
    if (!ctx->enc_ctx || !ctx->dec_ctx) {
        pthread_mutex_unlock(&ctx->lock);
        return -2;
    }

    memcpy(ctx->raw_key, key32, CRYPTO_KEY_LEN);
    ctx->local_sender_id = sender_id;
    __atomic_store_n(&ctx->tx_seq, 0, __ATOMIC_SEQ_CST);
    memset(ctx->replay, 0, sizeof(ctx->replay));
    ctx->ready = 1;
    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static inline int crypto_seal(CryptoContext *ctx,
                              const uint8_t *aad_prefix, uint32_t aad_prefix_len,
                              const uint8_t *plain, uint32_t plain_len,
                              uint8_t *nonce_out,
                              uint8_t *cipher_out,
                              uint8_t *tag_out) {
    if (!ctx->ready) return -1;
    uint64_t seq = __atomic_add_fetch(&ctx->tx_seq, 1, __ATOMIC_SEQ_CST);

    write_be32(nonce_out, ctx->local_sender_id);
    write_be64(nonce_out + 4, seq);

    uint8_t aad[64];
    if (aad_prefix_len + CRYPTO_NONCE_LEN > sizeof(aad)) return -2;
    if (aad_prefix_len > 0) memcpy(aad, aad_prefix, aad_prefix_len);
    memcpy(aad + aad_prefix_len, nonce_out, CRYPTO_NONCE_LEN);

    int len = 0, cipher_len = 0, final_len = 0;
    int ok = 1;

    pthread_mutex_lock(&ctx->lock);
    EVP_CIPHER_CTX_reset(ctx->enc_ctx);
    if (EVP_EncryptInit_ex(ctx->enc_ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) ok = 0;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx->enc_ctx, EVP_CTRL_GCM_SET_IVLEN, CRYPTO_NONCE_LEN, NULL) != 1) ok = 0;
    if (ok && EVP_EncryptInit_ex(ctx->enc_ctx, NULL, NULL, ctx->raw_key, nonce_out) != 1) ok = 0;
    if (ok && EVP_EncryptUpdate(ctx->enc_ctx, NULL, &len, aad, (int)(aad_prefix_len + CRYPTO_NONCE_LEN)) != 1) ok = 0;
    if (ok && EVP_EncryptUpdate(ctx->enc_ctx, cipher_out, &cipher_len, plain, (int)plain_len) != 1) ok = 0;
    if (ok && EVP_EncryptFinal_ex(ctx->enc_ctx, cipher_out + cipher_len, &final_len) != 1) ok = 0;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx->enc_ctx, EVP_CTRL_GCM_GET_TAG, CRYPTO_TAG_LEN, tag_out) != 1) ok = 0;
    pthread_mutex_unlock(&ctx->lock);

    return (ok && (uint32_t)(cipher_len + final_len) == plain_len) ? 0 : -3;
}

static inline int crypto_open(CryptoContext *ctx,
                              const uint8_t *aad_prefix, uint32_t aad_prefix_len,
                              const uint8_t *nonce,
                              const uint8_t *cipher, uint32_t cipher_len,
                              const uint8_t *tag,
                              uint8_t *plain_out,
                              uint32_t *sender_id_out) {
    if (!ctx->ready) return -1;
    uint32_t sid = read_be32(nonce);
    uint64_t seq = read_be64(nonce + 4);

    uint8_t aad[64];
    if (aad_prefix_len + CRYPTO_NONCE_LEN > sizeof(aad)) return -2;
    if (aad_prefix_len > 0) memcpy(aad, aad_prefix, aad_prefix_len);
    memcpy(aad + aad_prefix_len, nonce, CRYPTO_NONCE_LEN);

    uint8_t local_tag[CRYPTO_TAG_LEN];
    memcpy(local_tag, tag, CRYPTO_TAG_LEN);

    pthread_mutex_lock(&ctx->lock);
    if (!crypto_replay_check_locked(ctx, sid, seq, 0)) {
        pthread_mutex_unlock(&ctx->lock);
        return -3; /* replay detectado */
    }

    int len = 0, out_len = 0, final_len = 0;
    int ok = 1;
    EVP_CIPHER_CTX_reset(ctx->dec_ctx);
    if (EVP_DecryptInit_ex(ctx->dec_ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) ok = 0;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx->dec_ctx, EVP_CTRL_GCM_SET_IVLEN, CRYPTO_NONCE_LEN, NULL) != 1) ok = 0;
    if (ok && EVP_DecryptInit_ex(ctx->dec_ctx, NULL, NULL, ctx->raw_key, nonce) != 1) ok = 0;
    if (ok && EVP_DecryptUpdate(ctx->dec_ctx, NULL, &len, aad, (int)(aad_prefix_len + CRYPTO_NONCE_LEN)) != 1) ok = 0;
    if (ok && EVP_DecryptUpdate(ctx->dec_ctx, plain_out, &out_len, cipher, (int)cipher_len) != 1) ok = 0;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx->dec_ctx, EVP_CTRL_GCM_SET_TAG, CRYPTO_TAG_LEN, local_tag) != 1) ok = 0;

    int ret = ok ? EVP_DecryptFinal_ex(ctx->dec_ctx, plain_out + out_len, &final_len) : -1;
    if (ret > 0 && (uint32_t)(out_len + final_len) == cipher_len) {
        crypto_replay_check_locked(ctx, sid, seq, 1);
        pthread_mutex_unlock(&ctx->lock);
        if (sender_id_out) *sender_id_out = sid;
        return 0;
    }

    OPENSSL_cleanse(plain_out, cipher_len);
    pthread_mutex_unlock(&ctx->lock);
    return -4; /* tag GCM inválida */
}

#endif /* _WIN32 */

#endif /* C_CRYPTO_H */
