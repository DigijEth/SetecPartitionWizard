// Source file encryption/decryption tool for Setec Partition Wizard.
//
// Encrypts C++ source files so they cannot be read from the repo or filesystem.
// Only the build system (which knows the key) can decrypt them for compilation.
//
// Usage:
//   src_cipher encrypt <key> <input_file> <output_file.enc>
//   src_cipher decrypt <key> <input_file.enc> <output_file>
//
// Encryption: XOR stream cipher with 256-round cascaded key derivation.
// File format: [8-byte magic "SPWSRC01"][4-byte original size][encrypted data][32-byte tag]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static constexpr char MAGIC[] = "SPWSRC01";
static constexpr size_t MAGIC_LEN = 8;
static constexpr size_t TAG_LEN = 32;

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

static std::vector<uint8_t> derive_keystream(const std::string& passphrase, size_t stream_len)
{
    // Derive key bytes from passphrase
    std::vector<uint8_t> key(passphrase.begin(), passphrase.end());

    // Ensure minimum key length
    while (key.size() < 64)
    {
        size_t old = key.size();
        key.resize(old + passphrase.size());
        for (size_t i = 0; i < passphrase.size(); i++)
            key[old + i] = passphrase[i] ^ (uint8_t)(old + i) ^ 0xC3;
    }

    // Expand to stream length
    std::vector<uint8_t> state = key;
    while (state.size() < stream_len + 64)
    {
        size_t old = state.size();
        state.resize(old + key.size());
        for (size_t i = 0; i < key.size(); i++)
            state[old + i] = key[i] ^ (uint8_t)(old + i);
    }

    // 256 rounds of cascaded mixing
    for (int round = 0; round < 256; round++)
    {
        mix_round(state.data(), state.size(), (uint8_t)round ^ key[round % key.size()]);
    }

    return std::vector<uint8_t>(state.begin(), state.begin() + stream_len);
}

static std::vector<uint8_t> compute_tag(const uint8_t* data, size_t len, const std::string& passphrase)
{
    auto stream = derive_keystream(passphrase + "_tag_verify", TAG_LEN + len);
    std::vector<uint8_t> tag(TAG_LEN);
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++)
    {
        acc ^= data[i];
        acc = (acc << 1) | (acc >> 7);
    }
    for (size_t i = 0; i < TAG_LEN; i++)
    {
        tag[i] = stream[i] ^ acc ^ (uint8_t)i;
    }
    return tag;
}

static int do_encrypt(const std::string& key, const std::string& inpath, const std::string& outpath)
{
    // Read input
    std::ifstream in(inpath, std::ios::binary);
    if (!in)
    {
        fprintf(stderr, "Cannot open input: %s\n", inpath.c_str());
        return 1;
    }
    std::vector<uint8_t> plaintext((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    in.close();

    // Derive keystream
    auto keystream = derive_keystream(key, plaintext.size());

    // Encrypt
    std::vector<uint8_t> ciphertext(plaintext.size());
    for (size_t i = 0; i < plaintext.size(); i++)
        ciphertext[i] = plaintext[i] ^ keystream[i];

    // Compute tag over ciphertext
    auto tag = compute_tag(ciphertext.data(), ciphertext.size(), key);

    // Write output: magic + size + ciphertext + tag
    std::ofstream out(outpath, std::ios::binary);
    if (!out)
    {
        fprintf(stderr, "Cannot open output: %s\n", outpath.c_str());
        return 1;
    }

    uint32_t orig_size = (uint32_t)plaintext.size();
    out.write(MAGIC, MAGIC_LEN);
    out.write(reinterpret_cast<const char*>(&orig_size), 4);
    out.write(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());
    out.write(reinterpret_cast<const char*>(tag.data()), TAG_LEN);

    printf("Encrypted %s -> %s (%zu -> %zu bytes)\n",
           inpath.c_str(), outpath.c_str(), plaintext.size(),
           MAGIC_LEN + 4 + ciphertext.size() + TAG_LEN);
    return 0;
}

static int do_decrypt(const std::string& key, const std::string& inpath, const std::string& outpath)
{
    std::ifstream in(inpath, std::ios::binary);
    if (!in)
    {
        fprintf(stderr, "Cannot open input: %s\n", inpath.c_str());
        return 1;
    }
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    in.close();

    // Validate minimum size and magic
    if (raw.size() < MAGIC_LEN + 4 + TAG_LEN)
    {
        fprintf(stderr, "File too small or corrupt\n");
        return 1;
    }

    if (memcmp(raw.data(), MAGIC, MAGIC_LEN) != 0)
    {
        fprintf(stderr, "Invalid file magic\n");
        return 1;
    }

    uint32_t orig_size = 0;
    memcpy(&orig_size, raw.data() + MAGIC_LEN, 4);

    size_t cipher_offset = MAGIC_LEN + 4;
    size_t cipher_len = raw.size() - MAGIC_LEN - 4 - TAG_LEN;

    if (cipher_len != orig_size)
    {
        fprintf(stderr, "Size mismatch: expected %u, got %zu\n", orig_size, cipher_len);
        return 1;
    }

    const uint8_t* ciphertext = raw.data() + cipher_offset;
    const uint8_t* file_tag = raw.data() + cipher_offset + cipher_len;

    // Verify tag
    auto expected_tag = compute_tag(ciphertext, cipher_len, key);
    if (memcmp(file_tag, expected_tag.data(), TAG_LEN) != 0)
    {
        fprintf(stderr, "Tag verification failed — wrong key or corrupt file\n");
        return 1;
    }

    // Decrypt
    auto keystream = derive_keystream(key, cipher_len);
    std::vector<uint8_t> plaintext(cipher_len);
    for (size_t i = 0; i < cipher_len; i++)
        plaintext[i] = ciphertext[i] ^ keystream[i];

    // Write output
    std::ofstream out(outpath, std::ios::binary);
    if (!out)
    {
        fprintf(stderr, "Cannot open output: %s\n", outpath.c_str());
        return 1;
    }
    out.write(reinterpret_cast<const char*>(plaintext.data()), plaintext.size());

    printf("Decrypted %s -> %s (%zu bytes)\n", inpath.c_str(), outpath.c_str(), plaintext.size());
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc != 5)
    {
        fprintf(stderr, "Usage: %s <encrypt|decrypt> <key> <input> <output>\n", argv[0]);
        return 1;
    }

    std::string mode = argv[1];
    std::string key = argv[2];
    std::string inpath = argv[3];
    std::string outpath = argv[4];

    if (mode == "encrypt")
        return do_encrypt(key, inpath, outpath);
    else if (mode == "decrypt")
        return do_decrypt(key, inpath, outpath);

    fprintf(stderr, "Unknown mode: %s (use 'encrypt' or 'decrypt')\n", mode.c_str());
    return 1;
}
