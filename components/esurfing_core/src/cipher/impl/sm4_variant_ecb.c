/**
 * SM4-variant ECB — 从 CVersion sm4_variant_ecb_android.c 移植
 * Algo Id: D755A536-B551-468C-BD87-322182B223D4
 *
 * 与 CBC 版共用相同的 S-box/FK/CK 和 L/L' 变换，ECB 模式 + PKCS7
 */

#include "cipher/CipherInterface.h"
#include "cipher/CipherUtils.h"
#include "utils/Logger.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t key[16];
} sm4_variant_ecb_data_t;

static const uint32_t SM4V_ECB_FK[4] = {0xA3B1BAC6, 0x56AA3350, 0x677D9197, 0xB27022DC};
static const uint32_t SM4V_ECB_CK[32] = {
    0x00070E15,0x1C232A31,0x383F464D,0x545B6269,0x70777E85,0x8C939AA1,0xA8AFB6BD,0xC4CBD2D9,
    0xE0E7EEF5,0xFC030A11,0x181F262D,0x343B4249,0x50575E65,0x6C737A81,0x888F969D,0xA4ABB2B9,
    0xC0C7CED5,0xDCE3EAF1,0xF8FF060D,0x141B2229,0x30373E45,0x4C535A61,0x686F767D,0x848B9299,
    0xA0A7AEB5,0xBCC3CAD1,0xD8DFE6ED,0xF4FB0209,0x10171E25,0x2C333A41,0x484F565D,0x646B7279
};
static const uint8_t SM4V_ECB_S[256] = {
    0xd6,0x90,0xe9,0xfe,0xcc,0xe1,0x3d,0xb7,0x16,0xb6,0x14,0xc2,0x28,0xfb,0x2c,0x05,
    0x2b,0x67,0x9a,0x76,0x2a,0xbe,0x04,0xc3,0xaa,0x44,0x13,0x26,0x49,0x86,0x06,0x99,
    0x9c,0x42,0x50,0xf4,0x91,0xef,0x98,0x7a,0x33,0x54,0x0b,0x43,0xed,0xcf,0xac,0x62,
    0xe4,0xb3,0x1c,0xa9,0xc9,0x08,0xe8,0x95,0x80,0xdf,0x94,0xfa,0x75,0x8f,0x3f,0xa6,
    0x47,0x07,0xa7,0xfc,0xf3,0x73,0x17,0xba,0x83,0x59,0x3c,0x19,0xe6,0x85,0x4f,0xa8,
    0x68,0x6b,0x81,0xb2,0x71,0x64,0xda,0x8b,0xf8,0xeb,0x0f,0x4b,0x70,0x56,0x9d,0x35,
    0x1e,0x24,0x0e,0x5e,0x63,0x58,0xd1,0xa2,0x25,0x22,0x7c,0x3b,0x01,0x21,0x78,0x87,
    0xd4,0x00,0x46,0x57,0x9f,0xd3,0x27,0x52,0x4c,0x36,0x02,0xe7,0xa0,0xc4,0xc8,0x9e,
    0xea,0xbf,0x8a,0xd2,0x40,0xc7,0x38,0xb5,0xa3,0xf7,0xf2,0xce,0xf9,0x61,0x15,0xa1,
    0xe0,0xae,0x5d,0xa4,0x9b,0x34,0x1a,0x55,0xad,0x93,0x32,0x30,0xf5,0x8c,0xb1,0xe3,
    0x1d,0xf6,0xe2,0x2e,0x82,0x66,0xca,0x60,0xc0,0x29,0x23,0xab,0x0d,0x53,0x4e,0x6f,
    0xd5,0xdb,0x37,0x45,0xde,0xfd,0x8e,0x2f,0x03,0xff,0x6a,0x72,0x6d,0x6c,0x5b,0x51,
    0x8d,0x1b,0xaf,0x92,0xbb,0xdd,0xbc,0x7f,0x11,0xd9,0x5c,0x41,0x1f,0x10,0x5a,0xd8,
    0x0a,0xc1,0x31,0x88,0xa5,0xcd,0x7b,0xbd,0x2d,0x74,0xd0,0x12,0xb8,0xe5,0xb4,0xb0,
    0x89,0x69,0x97,0x4a,0x0c,0x96,0x77,0x7e,0x65,0xb9,0xf1,0x09,0xc5,0x6e,0xc6,0x84,
    0x18,0xf0,0x7d,0xec,0x3a,0xdc,0x4d,0x20,0x79,0xee,0x5f,0x3e,0xd7,0xcb,0x39,0x48
};

static uint32_t sm4e_bswap32(uint32_t x)
{
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) | ((x >> 8) & 0xFF00) | (x >> 24);
}
static uint32_t sm4e_rd32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void sm4e_wr32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void sm4e_keysched(const uint8_t key[16], uint32_t rk[32], int mode)
{
    uint32_t K[4];
    for (int i = 0; i < 4; ++i)
        K[i] = sm4e_bswap32(((const uint32_t *)key)[i]) ^ SM4V_ECB_FK[i];
    for (int i = 0; i < 32; ++i)
    {
        uint32_t t = K[1] ^ K[2] ^ K[3] ^ SM4V_ECB_CK[i];
        uint32_t s0 = SM4V_ECB_S[t >> 24], s1 = SM4V_ECB_S[(t >> 16) & 0xFF],
                 s2 = SM4V_ECB_S[(t >> 8) & 0xFF], s3 = SM4V_ECB_S[t & 0xFF];
        uint32_t w4 = (s0 << 24) | (s1 << 16);
        uint32_t w1 = w4 | (s2 << 8);
        uint32_t B = w1 | s3;
        uint32_t e1 = ((B << 13) | (w4 >> 19)) & 0xFFFFFFFFu;
        uint32_t e2 = ((B << 23) | (w1 >> 9)) & 0xFFFFFFFFu;
        rk[i] = B ^ K[0] ^ e1 ^ e2;
        K[0] = K[1]; K[1] = K[2]; K[2] = K[3]; K[3] = rk[i];
    }
    if (mode == 0)
    {
        for (int i = 0; i < 16; ++i)
        {
            uint32_t tmp = rk[i]; rk[i] = rk[31 - i]; rk[31 - i] = tmp;
        }
    }
}

static void sm4e_block(uint32_t rk[32], const uint8_t in[16], uint8_t out[16])
{
    uint32_t X[4] = { sm4e_rd32be(in), sm4e_rd32be(in + 4),
                      sm4e_rd32be(in + 8), sm4e_rd32be(in + 12) };
    uint32_t store[32];
    for (int i = 0; i < 32; ++i)
    {
        uint32_t t = X[1] ^ X[2] ^ X[3] ^ rk[i];
        uint32_t s0 = SM4V_ECB_S[t >> 24], s1 = SM4V_ECB_S[(t >> 16) & 0xFF],
                 s2 = SM4V_ECB_S[(t >> 8) & 0xFF], s3 = SM4V_ECB_S[t & 0xFF];
        uint32_t w4 = (s0 << 24) | (s1 << 16);
        uint32_t w1 = w4 | (s2 << 8);
        uint32_t B = w1 | s3;
        uint32_t e0 = (s0 >> 6) | ((B & 0x3FFFFFFF) << 2);
        uint32_t e1 = ((B << 24) | (w1 >> 8)) & 0xFFFFFFFFu;
        uint32_t e2 = ((B << 10) | (w4 >> 22)) & 0xFFFFFFFFu;
        uint32_t e3 = ((B << 18) | (w1 >> 14)) & 0xFFFFFFFFu;
        uint32_t nx = B ^ X[0] ^ e1 ^ e0 ^ e2 ^ e3;
        store[i] = nx;
        X[0] = X[1]; X[1] = X[2]; X[2] = X[3]; X[3] = nx;
    }
    sm4e_wr32be(out, store[31]);
    sm4e_wr32be(out + 4, store[30]);
    sm4e_wr32be(out + 8, store[29]);
    sm4e_wr32be(out + 12, store[28]);
}

static char* sm4_variant_ecb_encrypt(cipher_interface_t* self, const char* text)
{
    if (!self || !text) return NULL;
    const sm4_variant_ecb_data_t* d = self->private_data;
    const size_t text_len = strlen(text);
    const int pad = (int)(16 - (text_len & 0xF));
    const int total = (int)text_len + pad;
    uint8_t *buf = calloc(1, (size_t)total);
    if (!buf) return NULL;
    memcpy(buf, text, text_len);
    memset(buf + text_len, pad, (size_t)pad);
    uint32_t rk[32];
    sm4e_keysched(d->key, rk, 1);
    for (int off = 0; off < total; off += 16)
        sm4e_block(rk, buf + off, buf + off);
    char* hex = bytes_2_hex(buf, (size_t)total);
    free(buf);
    /* 调试: 输出 key 和加密结果前 32 hex 字符, 与 Python 参考对比 */
    LOG_INFO("[SM4v-ECB] key=%02X%02X%02X%02X..., text_len=%zu, pad=%d, total=%d",
             d->key[0], d->key[1], d->key[2], d->key[3], text_len, pad, total);
    if (hex) LOG_INFO("[SM4v-ECB] hex(前32): %.32s, len=%zu", hex, strlen(hex));

    /* Self-test: 用 key 加密已知明文, 比对 Python 参考值 */
    {
        static bool tested = false;
        if (!tested)
        {
            tested = true;
            const uint8_t test_key[16] = {0x53,0x2F,0x79,0x4A,0x4E,0x79,0x74,0x4D,
                                           0x67,0x66,0x57,0x5A,0x2D,0x44,0x5C,0x57};
            const char test_plain[] = "Hello, SM4-variant ECB test!";
            /* Python 参考: CA8438E5A6C1ED36FBEC68B9C9990B18EA17127A000E8D00B9810526D0324A3F */
            uint8_t test_buf[32];
            memcpy(test_buf, test_plain, 28);
            memset(test_buf + 28, 4, 4); /* PKCS7 pad=4 */
            uint32_t test_rk[32];
            sm4e_keysched(test_key, test_rk, 1);
            for (int off = 0; off < 32; off += 16)
                sm4e_block(test_rk, test_buf + off, test_buf + off);
            char test_hex[65];
            for (int i = 0; i < 32; i++)
                sprintf(&test_hex[i*2], "%02X", test_buf[i]);
            test_hex[64] = '\0';
            LOG_INFO("[SM4v-ECB SELF-TEST] %s", test_hex);
            LOG_INFO("[SM4v-ECB EXPECTED  ] CA8438E5A6C1ED36FBEC68B9C9990B18EA17127A000E8D00B9810526D0324A3F");
        }
    }
    return hex;
}

static char* sm4_variant_ecb_decrypt(cipher_interface_t* self, const char* hex)
{
    if (!self || !hex) return NULL;
    const sm4_variant_ecb_data_t* d = self->private_data;
    size_t bytes_len = 0;
    uint8_t* bytes = hex_2_bytes(hex, &bytes_len);
    if (!bytes) return NULL;
    if ((bytes_len & 0xF) != 0 || bytes_len < 16) { free(bytes); return NULL; }
    uint8_t *buf = malloc(bytes_len);
    if (!buf) { free(bytes); return NULL; }
    memcpy(buf, bytes, bytes_len);
    free(bytes);
    uint32_t rk[32];
    sm4e_keysched(d->key, rk, 0);
    for (size_t off = 0; off < bytes_len; off += 16)
        sm4e_block(rk, buf + off, buf + off);
    const int pad = buf[bytes_len - 1];
    if (pad < 1 || pad > 16) { free(buf); return NULL; }
    size_t out_len = bytes_len - (size_t)pad;
    char* result = malloc(out_len + 1);
    if (!result) { free(buf); return NULL; }
    memcpy(result, buf, out_len);
    result[out_len] = '\0';
    free(buf);
    return result;
}

static void sm4_variant_ecb_destroy(cipher_interface_t* self)
{
    if (self) { free(self->private_data); free(self); }
}

cipher_interface_t* create_sm4_variant_ecb_android_cipher(const uint8_t* key)
{
    if (!key) return NULL;
    cipher_interface_t* ci = calloc(1, sizeof(cipher_interface_t));
    sm4_variant_ecb_data_t* d = malloc(sizeof(sm4_variant_ecb_data_t));
    if (!ci || !d) { free(ci); free(d); return NULL; }
    memcpy(d->key, key, 16);
    ci->encrypt = sm4_variant_ecb_encrypt;
    ci->decrypt = sm4_variant_ecb_decrypt;
    ci->destroy = sm4_variant_ecb_destroy;
    ci->private_data = d;
    return ci;
}
