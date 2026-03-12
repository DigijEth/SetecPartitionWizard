#include "Types.h"

#include <cstring>
#include <cstdio>
#include <random>

namespace spw
{

bool Guid::operator==(const Guid& other) const
{
    return std::memcmp(data, other.data, 16) == 0;
}

bool Guid::operator!=(const Guid& other) const
{
    return !(*this == other);
}

bool Guid::isZero() const
{
    for (int i = 0; i < 16; ++i)
    {
        if (data[i] != 0)
            return false;
    }
    return true;
}

std::string Guid::toString() const
{
    // Standard GUID format: {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
    // Windows GUIDs store the first three groups in little-endian
    char buf[64];
    std::snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        data[3], data[2], data[1], data[0],  // Data1 (LE)
        data[5], data[4],                     // Data2 (LE)
        data[7], data[6],                     // Data3 (LE)
        data[8], data[9],                     // Data4[0..1]
        data[10], data[11], data[12], data[13], data[14], data[15]);
    return std::string(buf);
}

Guid Guid::fromString(const std::string& str)
{
    Guid g{};
    unsigned int d[16]{};
    // Parse "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
    if (std::sscanf(str.c_str(),
        "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
        &d[3], &d[2], &d[1], &d[0],
        &d[5], &d[4],
        &d[7], &d[6],
        &d[8], &d[9],
        &d[10], &d[11], &d[12], &d[13], &d[14], &d[15]) == 16)
    {
        for (int i = 0; i < 16; ++i)
            g.data[i] = static_cast<uint8_t>(d[i]);
    }
    return g;
}

Guid Guid::generate()
{
    Guid g{};
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    for (int i = 0; i < 16; ++i)
        g.data[i] = static_cast<uint8_t>(dist(gen));

    // Set version 4 (random) — bits 48-51 = 0100
    g.data[7] = static_cast<uint8_t>((g.data[7] & 0x0F) | 0x40);
    // Set variant 1 — bits 64-65 = 10
    g.data[8] = static_cast<uint8_t>((g.data[8] & 0x3F) | 0x80);

    return g;
}

} // namespace spw
