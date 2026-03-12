#pragma once

// Compile-time string obfuscation for sensitive UI strings.
// All pentesting menu text is stored XOR-encrypted and decoded at runtime.

#include <array>
#include <cstdint>
#include <string>

namespace spw
{
namespace obf
{

// Compile-time XOR key derived from line number + counter
constexpr uint8_t xor_key(size_t idx, uint8_t seed)
{
    return static_cast<uint8_t>((seed ^ 0xA7) + idx * 0x6D + (idx >> 2) * 0x3B);
}

template <size_t N>
struct ObfString
{
    uint8_t data[N] = {};
    uint8_t seed = 0;

    constexpr ObfString(const char (&str)[N], uint8_t s) : seed(s)
    {
        for (size_t i = 0; i < N; i++)
        {
            data[i] = static_cast<uint8_t>(str[i]) ^ xor_key(i, seed);
        }
    }

    std::string decode() const
    {
        std::string result(N - 1, '\0');
        for (size_t i = 0; i < N - 1; i++)
        {
            result[i] = static_cast<char>(data[i] ^ xor_key(i, seed));
        }
        return result;
    }

    QString qdecode() const
    {
        return QString::fromStdString(decode());
    }
};

// Macro: OBF("string") creates a compile-time encrypted string
// The __LINE__ is used as the seed so each usage gets a different key
#define OBF(str) ([]() { \
    constexpr ::spw::obf::ObfString<sizeof(str)> _obf(str, (uint8_t)(__LINE__ ^ 0x55)); \
    return _obf; \
}())

// Convenience: OBFS returns std::string, OBFQ returns QString
#define OBFS(str) OBF(str).decode()
#define OBFQ(str) OBF(str).qdecode()

} // namespace obf
} // namespace spw
