// Small AES-128 (encryption only), SHA-1 and MD5, enough for the UDS beacon's encrypted node list.
#include <cstring>
#include "cryptopp/mini.h"

namespace CryptoPP {
namespace {

constexpr byte sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

byte Xtime(byte x) {
    return static_cast<byte>((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

std::uint32_t Rol(std::uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

} // namespace

void Aes128Encrypt(const byte key[16], const byte in[16], byte out[16]) {
    byte rk[176];
    std::memcpy(rk, key, 16);
    byte rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        byte t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0) {
            const byte first = t[0];
            t[0] = static_cast<byte>(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = Xtime(rcon);
        }
        for (int j = 0; j < 4; j++) {
            rk[i + j] = rk[i - 16 + j] ^ t[j];
        }
    }
    byte s[16];
    for (int i = 0; i < 16; i++) {
        s[i] = in[i] ^ rk[i];
    }
    for (int round = 1; round <= 10; round++) {
        byte t[16];
        for (int i = 0; i < 16; i++) {
            t[i] = sbox[s[(i + 4 * (i % 4)) % 16]]; // SubBytes + ShiftRows
        }
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                byte* col = t + 4 * c;
                const byte a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                const byte all = a0 ^ a1 ^ a2 ^ a3;
                col[0] ^= all ^ Xtime(a0 ^ a1);
                col[1] ^= all ^ Xtime(a1 ^ a2);
                col[2] ^= all ^ Xtime(a2 ^ a3);
                col[3] ^= all ^ Xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++) {
            s[i] = t[i] ^ rk[16 * round + i];
        }
    }
    std::memcpy(out, s, 16);
}

void Sha1(const byte* data, std::size_t size, byte out[20]) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const std::size_t total = ((size + 9 + 63) / 64) * 64;
    for (std::size_t block = 0; block < total; block += 64) {
        byte chunk[64];
        for (int i = 0; i < 64; i++) {
            const std::size_t k = block + i;
            if (k < size) {
                chunk[i] = data[k];
            } else if (k == size) {
                chunk[i] = 0x80;
            } else if (k >= total - 8) {
                chunk[i] = static_cast<byte>(static_cast<std::uint64_t>(size) * 8 >> (8 * (total - 1 - k)));
            } else {
                chunk[i] = 0;
            }
        }
        std::uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (std::uint32_t(chunk[4 * i]) << 24) | (std::uint32_t(chunk[4 * i + 1]) << 16) |
                   (std::uint32_t(chunk[4 * i + 2]) << 8) | chunk[4 * i + 3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d), k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d, k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d, k = 0xCA62C1D6;
            }
            const std::uint32_t t = Rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = Rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    for (int i = 0; i < 20; i++) {
        out[i] = static_cast<byte>(h[i / 4] >> (24 - 8 * (i % 4)));
    }
}

void Md5(const byte* data, std::size_t size, byte out[16]) {
    static const std::uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const std::size_t total = ((size + 9 + 63) / 64) * 64;
    for (std::size_t block = 0; block < total; block += 64) {
        byte chunk[64];
        for (int i = 0; i < 64; i++) {
            const std::size_t k = block + i;
            if (k < size) {
                chunk[i] = data[k];
            } else if (k == size) {
                chunk[i] = 0x80;
            } else if (k >= total - 8) {
                chunk[i] = static_cast<byte>(static_cast<std::uint64_t>(size) * 8 >> (8 * (k - (total - 8))));
            } else {
                chunk[i] = 0;
            }
        }
        std::uint32_t m[16];
        for (int i = 0; i < 16; i++) {
            m[i] = chunk[4 * i] | (std::uint32_t(chunk[4 * i + 1]) << 8) |
                   (std::uint32_t(chunk[4 * i + 2]) << 16) | (std::uint32_t(chunk[4 * i + 3]) << 24);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            std::uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d), g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d, g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d), g = (7 * i) % 16;
            }
            const std::uint32_t t = d;
            d = c, c = b;
            b = b + Rol(a + f + K[i] + m[g], R[i]);
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d;
    }
    for (int i = 0; i < 16; i++) {
        out[i] = static_cast<byte>(h[i / 4] >> (8 * (i % 4)));
    }
}

} // namespace CryptoPP
