#pragma once

// FormatEngine — Format partitions/volumes to any supported filesystem.
//
// For Windows-native formats (NTFS, FAT32<=32GB, exFAT, ReFS), delegates to
// format.com or DeviceIoControl. For Linux filesystems (ext2/3/4, swap) and
// large FAT32 (>32GB), writes on-disk structures directly.
//
// All operations lock and dismount the volume before writing.
// Supports quick format (structures only) and full format (zero + structures).
//
// DISCLAIMER: This code is for authorized disk utility software only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../common/Constants.h"
#include "../disk/VolumeHandle.h"
#include "../disk/RawDiskHandle.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <QString>

namespace spw
{

// Progress callback: (percent 0-100, status message)
using FormatProgressCallback = std::function<void(int percent, const QString& status)>;

// Options controlling format behavior
struct FormatOptions
{
    FilesystemType  targetFs = FilesystemType::NTFS;
    std::string     volumeLabel;           // Volume label (max length depends on FS)
    bool            quickFormat = true;     // false = zero entire partition first
    uint32_t        clusterSize = 0;       // 0 = auto-select based on volume size
    bool            enableCompression = false; // NTFS only
    bool            enableJournal = true;  // ext3/ext4: enable journal; ext2: ignored

    // ext-family specific
    uint32_t        inodeSize = 256;       // ext2/3/4 inode size (128 or 256)
    uint32_t        inodesPerGroup = 0;    // 0 = auto
    uint32_t        blockSize = 0;         // ext block size (1024/2048/4096), 0 = auto
    bool            enable64bit = true;    // ext4: enable 64-bit feature
    bool            enableExtents = true;  // ext4: enable extents
    bool            enableHugeFile = true; // ext4: enable huge_file

    // Linux swap specific
    uint32_t        swapPageSize = 4096;   // Usually 4096

    // FAT32 large (>32GB) specific — bypass Windows limitation
    bool            forceFat32Large = false;
};

// Describes a volume to format — either by drive letter or by raw disk + offset
struct FormatTarget
{
    // Option A: Format by drive letter (Windows volumes)
    wchar_t driveLetter = 0;

    // Option B: Format by raw disk + partition offset/size (for Linux FS, no mount point)
    DiskId  diskIndex = -1;
    uint64_t partitionOffsetBytes = 0;
    uint64_t partitionSizeBytes = 0;
    uint32_t sectorSize = SECTOR_SIZE_512;

    // Returns true if targeting a drive letter
    bool hasDriveLetter() const { return driveLetter != 0; }

    // Returns true if targeting raw disk
    bool hasRawTarget() const { return diskIndex >= 0 && partitionSizeBytes > 0; }
};

class FormatEngine
{
public:
    FormatEngine() = default;
    ~FormatEngine() = default;

    // Non-copyable
    FormatEngine(const FormatEngine&) = delete;
    FormatEngine& operator=(const FormatEngine&) = delete;

    // Format a partition/volume with the given options.
    // This is the main entry point — it dispatches to the appropriate formatter.
    Result<void> format(const FormatTarget& target,
                        const FormatOptions& options,
                        FormatProgressCallback progress = nullptr);

    // Query whether a filesystem type is supported for formatting
    static bool isFormatSupported(FilesystemType fs);

    // Get the recommended cluster/block size for a filesystem and volume size
    static uint32_t recommendedClusterSize(FilesystemType fs, uint64_t volumeSizeBytes);

    // Get maximum volume label length for a filesystem
    static int maxLabelLength(FilesystemType fs);

private:
    // ----- Windows-native formatters (delegate to format.com) -----
    Result<void> formatWithWindowsTool(const FormatTarget& target,
                                       const FormatOptions& options,
                                       FormatProgressCallback progress);

    // ----- Direct-write formatters -----

    // ext2/ext3/ext4 — write superblock, group descriptors, bitmaps, inode table, root dir
    Result<void> formatExt(const FormatTarget& target,
                           const FormatOptions& options,
                           FormatProgressCallback progress);

    // FAT32 for volumes >32GB (Windows refuses)
    Result<void> formatFat32Large(const FormatTarget& target,
                                  const FormatOptions& options,
                                  FormatProgressCallback progress);

    // Linux swap — write swap header with UUID and SWAPSPACE2 magic
    Result<void> formatLinuxSwap(const FormatTarget& target,
                                 const FormatOptions& options,
                                 FormatProgressCallback progress);

    // ----- Helpers -----

    // Zero the entire volume (for full format)
    Result<void> zeroVolume(VolumeHandle& vol, uint64_t totalBytes,
                            FormatProgressCallback progress,
                            int progressStart, int progressEnd);

    // Zero via raw disk handle
    Result<void> zeroRaw(RawDiskHandle& disk, uint64_t offsetBytes,
                         uint64_t totalBytes, uint32_t sectorSize,
                         FormatProgressCallback progress,
                         int progressStart, int progressEnd);

    // Lock and dismount a volume by drive letter, returning the handle
    Result<VolumeHandle> lockAndDismount(wchar_t driveLetter);

    // Notify the OS that partition geometry changed
    static Result<void> notifyPartitionChange(DiskId diskIndex);
    static Result<void> notifyPartitionChangeLetter(wchar_t driveLetter);
};

} // namespace spw
