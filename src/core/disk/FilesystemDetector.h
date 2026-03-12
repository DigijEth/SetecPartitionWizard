#pragma once

// FilesystemDetector — Identifies filesystem type by reading magic bytes and structural signatures.
//
// Checks a comprehensive set of filesystem signatures covering modern, legacy, and exotic
// filesystem types. Detection works by reading specific byte offsets from the target volume
// and matching against known magic values.
//
// DISCLAIMER: This code is for authorized disk utility software only.

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../common/Constants.h"
#include "PartitionTable.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spw
{

// Summary of detection result
struct FilesystemDetection
{
    FilesystemType type = FilesystemType::Unknown;
    std::string    label;             // Volume label if readable during detection
    std::string    uuid;              // UUID/serial if readable
    uint32_t       blockSize = 0;     // Block/cluster size if determinable
    std::string    description;       // Human-readable filesystem name

    bool isDetected() const { return type != FilesystemType::Unknown; }
};

// ============================================================================
// FilesystemDetector — static methods for filesystem identification
// ============================================================================
class FilesystemDetector
{
public:
    // Detect the filesystem type present at the given read callback.
    // The callback reads raw bytes from the start of the volume/partition.
    // Parameters:
    //   readFunc — reads (offset, size) relative to partition/volume start
    //   volumeSize — total size of partition in bytes (0 if unknown)
    static Result<FilesystemDetection> detect(
        const DiskReadCallback& readFunc,
        uint64_t volumeSize = 0);

    // Detect from a raw buffer (useful for testing or when data is already in memory).
    // The buffer should contain at least the first 128 KiB of the volume for reliable detection.
    // For Btrfs, 72 KiB is needed (superblock at 0x10000 + 64 bytes).
    static Result<FilesystemDetection> detectFromBuffer(
        const std::vector<uint8_t>& data,
        uint64_t volumeSize = 0);

    // Get a human-readable name for a FilesystemType
    static const char* filesystemName(FilesystemType type);

private:
    // Individual detection routines — each returns true if the filesystem was positively identified
    static bool detectNtfs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectFat(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectExfat(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectExt(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectBtrfs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectXfs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectHfsPlus(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectApfs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectReFs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectIso9660(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectUdf(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectReiserFs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectJfs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectHpfs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectMinix(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectUfs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectBfs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectQnx4(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectZfs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectSquashFs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectCramFs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectRomFs(const DiskReadCallback& readFunc, FilesystemDetection& out);
    static bool detectLinuxSwap(const DiskReadCallback& readFunc, FilesystemDetection& out, uint64_t volumeSize);

    // Helper: safely read bytes through the callback, returning empty vector on failure
    static std::vector<uint8_t> safeRead(const DiskReadCallback& readFunc, uint64_t offset, uint32_t size);
};

} // namespace spw
