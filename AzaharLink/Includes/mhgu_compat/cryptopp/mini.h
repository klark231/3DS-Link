#pragma once
// The few Crypto++ pieces the UDS beacon code uses (AES-128-CTR, SHA-1, MD5), as small standalone code.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace CryptoPP {
using byte = unsigned char;

void Sha1(const byte* data, std::size_t size, byte out[20]);
void Md5(const byte* data, std::size_t size, byte out[16]);
void Aes128Encrypt(const byte key[16], const byte in[16], byte out[16]);

struct AES {
    static constexpr std::size_t BLOCKSIZE = 16;
};

struct SHA1 {
    static constexpr std::size_t DIGESTSIZE = 20;
    void CalculateDigest(byte* out, const byte* in, std::size_t size) {
        Sha1(in, size, out);
    }
};

namespace Weak {
struct MD5 {
    static constexpr std::size_t DIGESTSIZE = 16;
    void CalculateDigest(byte* out, const byte* in, std::size_t size) {
        Md5(in, size, out);
    }
};
} // namespace Weak

class CtrCipher {
public:
    void SetKeyWithIV(const byte* k, std::size_t, const byte* iv) {
        std::memcpy(key, k, 16);
        std::memcpy(counter, iv, 16);
        used = 16;
    }
    void ProcessData(byte* out, const byte* in, std::size_t size) {
        for (std::size_t i = 0; i < size; i++) {
            if (used == 16) {
                Aes128Encrypt(key, counter, stream);
                for (int j = 15; j >= 0 && ++counter[j] == 0; j--) {
                }
                used = 0;
            }
            out[i] = in[i] ^ stream[used++];
        }
    }

private:
    byte key[16]{}, counter[16]{}, stream[16]{};
    std::size_t used = 16;
};

template <typename Cipher>
struct CTR_Mode {
    using Encryption = CtrCipher;
    using Decryption = CtrCipher;
};
} // namespace CryptoPP
