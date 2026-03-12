#pragma once

// FilesystemInfo — Reads detailed filesystem metadata after detection.
//
// Once FilesystemDetector identifies a filesystem type, this class reads the
// on-disk structures to extract label, UUID, size, free space, features, and
// version information. Each filesystem has its own superblock/BPB layout.
//
// DISCLAIMER: This code is for authorized disk utility software only.

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "PartitionTable.h"
#include "FilesystemDetector.h"

#include <cstdint>
#include <string>
#include <vector>

namespace spw
{

// Feature flags for ext2/3/4 (a selection of the most important ones)
namespace ExtFeatures
{
    constexpr uint32_t Compat_HasJournal    = 0x0004;
    constexpr uint32_t Compat_ExtAttr       = 0x0010;
    constexpr uint32_t Compat_ResizeInode   = 0x0010;
    constexpr uint32_t Compat_DirIndex      = 0x0020;
    constexpr uint32_t Incompat_Filetype    = 0x0002;
    constexpr uint32_t Incompat_Recover     = 0x0004;
    constexpr uint32_t Incompat_Extents     = 0x0040;
    constexpr uint32_t Incompat_64bit       = 0x0080;
    constexpr uint32_t Incompat_FlexBg      = 0x0200;
    constexpr uint32_t RoCompat_Sparse      = 0x0001;
    constexpr uint32_t RoCompat_LargeFile   = 0x0002;
    constexpr uint32_t RoCompat_HugeFile    = 0x0008;
    constexpr uint32_t RoCompat_Metadata    = 0x1000;
}

// ============================================================================
// Detailed filesystem information
// ============================================================================
struct FilesystemInfoData
{
    // Basic identification
    FilesystemType type = FilesystemType::Unknown;
    std::string    typeName;           // Human-readable type name
    std::string    label;              // Volume label / name
    std::string    uuid;               // UUID, serial number, or equivalent

    // Size information
    uint32_t blockSize = 0;            // Block/cluster size in bytes
    uint64_t totalBlocks = 0;          // Total blocks/clusters
    uint64_t freeBlocks = 0;           // Free blocks (if readable from superblock)
    uint64_t totalSizeBytes = 0;       // Total filesystem size in bytes
    uint64_t freeSizeBytes = 0;        // Free space in bytes (0 if unknown)
    uint64_t usedSizeBytes = 0;        // Used space in bytes

    // NTFS-specific
    struct
    {
        uint8_t majorVersion = 0;      // NTFS version (e.g. 3.1)
        uint8_t minorVersion = 0;
        uint64_t mftCluster = 0;       // Starting cluster of $MFT
        uint64_t mftMirrorCluster = 0; // Starting cluster of $MFTMirr
        uint32_t mftRecordSize = 0;    // Bytes per MFT record
        uint64_t serialNumber = 0;     // Volume serial number
    } ntfs;

    // FAT-specific
    struct
    {
        uint8_t  fatCount = 0;         // Number of FAT copies
        uint32_t fatSize = 0;          // Sectors per FAT
        uint16_t reservedSectors = 0;
        uint16_t rootEntryCount = 0;   // FAT12/16 root dir entries
        uint32_t totalClusters = 0;
        uint32_t volumeSerial = 0;
        std::string oemName;           // OEM name from BPB (8 bytes)
    } fat;

    // ext-specific
    struct
    {
        uint32_t inodeCount = 0;
        uint32_t freeInodes = 0;
        uint32_t blockGroupCount = 0;
        uint32_t compatFeatures = 0;
        uint32_t incompatFeatures = 0;
        uint32_t roCompatFeatures = 0;
        uint16_t state = 0;            // 1 = clean, 2 = errors
        uint16_t errors = 0;           // Behavior on error
        uint32_t creatorOs = 0;        // 0=Linux, 1=Hurd, 2=Masix, 3=FreeBSD, 4=Lites
        std::vector<std::string> featureStrings; // Human-readable feature list
    } ext;

    // exFAT-specific
    struct
    {
        uint16_t fsRevision = 0;
        uint32_t clusterCount = 0;
        uint32_t volumeSerial = 0;
    } exfat;

    // HFS+ specific
    struct
    {
        uint16_t version = 0;          // 4 = HFS+, 5 = HFSX
        uint32_t fileCount = 0;
        uint32_t folderCount = 0;
    } hfsplus;
};

// ============================================================================
// FilesystemInfo — reads detailed metadata from a detected filesystem
// ============================================================================
class FilesystemInfo
{
public:
    // Read detailed metadata for a filesystem that was already detected.
    // readFunc reads raw bytes from the start of the volume/partition.
    static Result<FilesystemInfoData> read(
        FilesystemType type,
        const DiskReadCallback& readFunc,
        uint64_t volumeSize = 0);

    // Convenience: detect and then read info in one call
    static Result<FilesystemInfoData> detectAndRead(
        const DiskReadCallback& readFunc,
        uint64_t volumeSize = 0);

private:
    static Result<FilesystemInfoData> readNtfs(const DiskReadCallback& readFunc, uint64_t volumeSize);
    static Result<FilesystemInfoData> readFat(const DiskReadCallback& readFunc, uint64_t volumeSize);
    static Result<FilesystemInfoData> readExfat(const DiskReadCallback& readFunc, uint64_t volumeSize);
    static Result<FilesystemInfoData> readExt(const DiskReadCallback& readFunc, uint64_t volumeSize, FilesystemType type);
    static Result<FilesystemInfoData> readBtrfs(const DiskReadCallback& readFunc, uint64_t volumeSize);
    static Result<FilesystemInfoData> readXfs(const DiskReadCallback& readFunc, uint64_t volumeSize);
    static Result<FilesystemInfoData> readHfsPlus(const DiskReadCallback& readFunc, uint64_t volumeSize);
    static Result<FilesystemInfoData> readApfs(const DiskReadCallback& readFunc, uint64_t volumeSize);
    static Result<FilesystemInfoData> readGeneric(FilesystemType type, const DiskReadCallback& readFunc, uint64_t volumeSize);

    // Helper: read N bytes safely
    static std::vector<uint8_t> safeRead(const DiskReadCallback& readFunc, uint64_t offset, uint32_t size);
};

} // namespace spw
