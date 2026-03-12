#pragma once

// Checksums — Cryptographic and non-cryptographic hash utilities for disk imaging.
// Uses Windows BCrypt API for SHA-256 and MD5. CRC32 is a pure software implementation.
// All operations support progress callbacks for hashing large disk regions.
// DISCLAIMER: This code is for authorized disk utility software only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <bcrypt.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../disk/RawDiskHandle.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spw
{

// Fixed-size hash results
using SHA256Hash = std::array<uint8_t, 32>;
using MD5Hash = std::array<uint8_t, 16>;

// Progress callback: (bytesProcessed, totalBytes) -> return false to cancel
using HashProgressCallback = std::function<bool(uint64_t bytesProcessed, uint64_t totalBytes)>;

// Convert hash bytes to lowercase hex string
std::string hashToHexString(const uint8_t* data, size_t length);
std::string sha256ToHex(const SHA256Hash& hash);
std::string md5ToHex(const MD5Hash& hash);

namespace Checksums
{

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

// Hash an in-memory buffer
Result<SHA256Hash> sha256Buffer(const uint8_t* data, size_t length);

// Hash an entire file on disk
Result<SHA256Hash> sha256File(const std::wstring& filePath,
                              HashProgressCallback progressCb = nullptr);

// Hash a range of sectors from a raw disk
Result<SHA256Hash> sha256DiskRange(const RawDiskHandle& disk,
                                   SectorOffset startLba,
                                   SectorCount sectorCount,
                                   uint32_t sectorSize,
                                   HashProgressCallback progressCb = nullptr);

// ---------------------------------------------------------------------------
// MD5 (for legacy image verification)
// ---------------------------------------------------------------------------

Result<MD5Hash> md5Buffer(const uint8_t* data, size_t length);

Result<MD5Hash> md5File(const std::wstring& filePath,
                        HashProgressCallback progressCb = nullptr);

Result<MD5Hash> md5DiskRange(const RawDiskHandle& disk,
                             SectorOffset startLba,
                             SectorCount sectorCount,
                             uint32_t sectorSize,
                             HashProgressCallback progressCb = nullptr);

// ---------------------------------------------------------------------------
// CRC32
// ---------------------------------------------------------------------------

// CRC32 (ISO 3309 / ITU-T V.42, same polynomial as zlib)
uint32_t crc32Buffer(const uint8_t* data, size_t length);

// Incremental CRC32: pass previous CRC (or 0 for first call)
uint32_t crc32Update(uint32_t previousCrc, const uint8_t* data, size_t length);

Result<uint32_t> crc32File(const std::wstring& filePath,
                           HashProgressCallback progressCb = nullptr);

Result<uint32_t> crc32DiskRange(const RawDiskHandle& disk,
                                SectorOffset startLba,
                                SectorCount sectorCount,
                                uint32_t sectorSize,
                                HashProgressCallback progressCb = nullptr);

} // namespace Checksums
} // namespace spw
