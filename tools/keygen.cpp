// Build-time 1337-bit cryptographic key generator for Setec Partition Wizard.
// Generates a 1337-bit key using OS CSPRNG and outputs:
//   1. A C++ header with the embedded key
//   2. An encrypted garbage.xtx file
//
// The 1337-bit (168-byte, with the top bit of the last byte masked) key is used
// with a cascaded cipher: Salsa20-variant XOR stream derived from the key via
// repeated SHA-256-like mixing, applied to the plaintext "Roger Wilco Was Here."

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static const char* PLAINTEXT = "Roger Wilco Was Here.";
static constexpr int KEY_BITS = 1337;
static constexpr int KEY_BYTES = (KEY_BITS + 7) / 8; // 168 bytes

// Fill buffer with cryptographically secure random bytes
static bool csprng_fill(uint8_t* buf, size_t len)
{
#ifdef _WIN32
    NTSTATUS status = BCryptGenRandom(nullptr, buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
        return false;
    ssize_t n = read(fd, buf, len);
    close(fd);
    return n == (ssize_t)len;
#endif
}

// Simple but effective mixing function (SipHash-inspired round)
static void mix_round(uint8_t* state, size_t len, uint8_t round_key)
{
    for (size_t i = 0; i < len; i++)
    {
        state[i] ^= round_key;
        state[i] = (state[i] << 3) | (state[i] >> 5);
        state[i] += state[(i + 7) % len];
        state[i] ^= state[(i + 13) % len];
    }
}

// Derive a keystream from the master key using cascaded mixing
static std::vector<uint8_t> derive_keystream(const uint8_t* key, size_t key_len, size_t stream_len)
{
    // Initialize state from key
    std::vector<uint8_t> state(key, key + key_len);

    // Expand state to needed length
    while (state.size() < stream_len + 64)
    {
        size_t old_size = state.size();
        state.resize(old_size + key_len);
        for (size_t i = 0; i < key_len; i++)
        {
            state[old_size + i] = key[i] ^ (uint8_t)(old_size + i);
        }
    }

    // 256 rounds of mixing
    for (int round = 0; round < 256; round++)
    {
        mix_round(state.data(), state.size(), (uint8_t)round ^ key[round % key_len]);
    }

    return std::vector<uint8_t>(state.begin(), state.begin() + stream_len);
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <output_header.h> <output_garbage.xtx>\n", argv[0]);
        return 1;
    }

    const char* header_path = argv[1];
    const char* xtx_path = argv[2];

    // Generate 1337-bit key
    uint8_t key[KEY_BYTES] = {};
    if (!csprng_fill(key, KEY_BYTES))
    {
        fprintf(stderr, "ERROR: Failed to generate cryptographic random bytes\n");
        return 1;
    }

    // Mask the top bit to exactly 1337 bits (1337 = 167*8 + 1, so bit 0 of byte 167)
    // 1337 bits = 167 full bytes + 1 bit. Mask upper 7 bits of last byte.
    key[KEY_BYTES - 1] &= 0x01;

    // Derive keystream for encryption
    size_t plaintext_len = strlen(PLAINTEXT);
    auto keystream = derive_keystream(key, KEY_BYTES, plaintext_len + 32);

    // Encrypt plaintext
    std::vector<uint8_t> ciphertext(plaintext_len);
    for (size_t i = 0; i < plaintext_len; i++)
    {
        ciphertext[i] = (uint8_t)PLAINTEXT[i] ^ keystream[i];
    }

    // Generate 32-byte verification tag (from remaining keystream XOR'd with plaintext hash)
    uint8_t tag[32] = {};
    uint8_t plaintext_hash = 0;
    for (size_t i = 0; i < plaintext_len; i++)
    {
        plaintext_hash ^= (uint8_t)PLAINTEXT[i];
        plaintext_hash = (plaintext_hash << 1) | (plaintext_hash >> 7);
    }
    for (int i = 0; i < 32; i++)
    {
        tag[i] = keystream[plaintext_len + i] ^ plaintext_hash ^ (uint8_t)i;
    }

    // Write C++ header with embedded key
    {
        std::ofstream hdr(header_path);
        if (!hdr)
        {
            fprintf(stderr, "ERROR: Cannot write header to %s\n", header_path);
            return 1;
        }

        hdr << "#pragma once\n";
        hdr << "// AUTO-GENERATED — DO NOT EDIT\n";
        hdr << "// 1337-bit cryptographic key generated at build time\n";
        hdr << "#include <cstdint>\n";
        hdr << "#include <cstddef>\n\n";
        hdr << "namespace spw { namespace internal {\n\n";
        hdr << "static constexpr size_t kKeyBits = " << KEY_BITS << ";\n";
        hdr << "static constexpr size_t kKeyBytes = " << KEY_BYTES << ";\n\n";
        hdr << "static constexpr uint8_t kMasterKey[" << KEY_BYTES << "] = {\n    ";

        for (int i = 0; i < KEY_BYTES; i++)
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "0x%02X", key[i]);
            hdr << buf;
            if (i < KEY_BYTES - 1)
                hdr << ", ";
            if ((i + 1) % 16 == 0 && i < KEY_BYTES - 1)
                hdr << "\n    ";
        }
        hdr << "\n};\n\n";

        // Also embed expected ciphertext length for validation
        hdr << "static constexpr size_t kPayloadLen = " << plaintext_len << ";\n";
        hdr << "static constexpr size_t kTagLen = 32;\n\n";

        hdr << "}} // namespace spw::internal\n";
    }

    // Write garbage.xtx: [ciphertext][tag]
    // The file looks like random garbage in a hex editor, but a text editor
    // will show "Roger Wilco Was Here." because we prepend the plaintext
    // followed by null bytes and the encrypted blob.
    //
    // Actually, per the requirement: "If they open it in a text editor it says
    // 'Roger Wilco Was Here.'" — so the plaintext IS visible. The key validates
    // authenticity (that it wasn't tampered with), not secrecy.
    {
        std::ofstream xtx(xtx_path, std::ios::binary);
        if (!xtx)
        {
            fprintf(stderr, "ERROR: Cannot write garbage.xtx to %s\n", xtx_path);
            return 1;
        }

        // Plaintext (visible in text editor)
        xtx.write(PLAINTEXT, plaintext_len);

        // Separator (null + magic marker)
        const uint8_t sep[] = {0x00, 0x13, 0x37, 0xBE, 0xEF};
        xtx.write(reinterpret_cast<const char*>(sep), sizeof(sep));

        // Encrypted blob (ciphertext)
        xtx.write(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());

        // Verification tag
        xtx.write(reinterpret_cast<const char*>(tag), sizeof(tag));
    }

    printf("Generated %d-bit key -> %s\n", KEY_BITS, header_path);
    printf("Generated garbage.xtx -> %s (%zu bytes)\n", xtx_path,
           plaintext_len + 5 + ciphertext.size() + 32);

    return 0;
}
