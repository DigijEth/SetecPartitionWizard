#pragma once

// SdCardRecovery — Detects and repairs SD cards that Windows cannot see.
// Handles cards with corrupted partition tables (e.g., from interrupted format),
// RAW/uninitialized cards, and cards with no valid filesystem.
// Uses raw disk I/O via IOCTL_DISK_CREATE_DISK and IOCTL_DISK_SET_DRIVE_LAYOUT_EX
// to reinitialize partition tables without relying on Windows volume management.

#include <windows.h>
#include <winioctl.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../disk/DiskEnumerator.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spw
{

class RawDiskHandle;

// Status of an SD card as detected by the recovery tool
enum class SdCardStatus
{
    Healthy,           // Card is fine, has valid partition table and filesystem
    NoPartitionTable,  // No valid MBR or GPT found
    CorruptPartition,  // Partition table exists but is damaged
    RawFilesystem,     // Partition exists but filesystem is RAW/unrecognized
    NoMedia,           // Card reader detected but no card inserted
    Unknown
};

// Information about a detected SD card (may or may not be visible to Windows)
struct SdCardInfo
{
    DiskId diskId = -1;
    std::wstring model;
    std::wstring serialNumber;
    uint64_t sizeBytes = 0;
    uint32_t sectorSize = 512;
    SdCardStatus status = SdCardStatus::Unknown;
    std::wstring statusDescription;
    bool hasPartitions = false;
    bool hasDriveLetter = false;
    wchar_t driveLetter = L'\0';
    DiskInterfaceType interfaceType = DiskInterfaceType::Unknown;
};

// What to do when fixing the card
enum class SdFixAction
{
    CleanAndFormat,      // Wipe partition table, create new MBR + single FAT32/exFAT partition
    ReinitPartitionOnly, // Just rewrite the partition table, don't format
    FormatOnly           // Keep partition table, just format the existing partition
};

struct SdFixConfig
{
    SdFixAction action = SdFixAction::CleanAndFormat;
    FilesystemType targetFs = FilesystemType::FAT32;  // FAT32 for <= 32GB, exFAT for > 32GB
    std::wstring volumeLabel = L"SD_CARD";
    bool quickFormat = true;
};

using SdProgressCallback = std::function<void(const std::string& stage, int percentComplete)>;

class SdCardRecovery
{
public:
    // Scan the system for SD/MMC card readers and cards, including those
    // that Windows doesn't assign drive letters to.
    // This uses SetupAPI directly, not just volume enumeration.
    static Result<std::vector<SdCardInfo>> detectSdCards();

    // Analyze a specific disk to determine its SD card status
    static Result<SdCardInfo> analyzeDisk(DiskId diskId);

    // Fix an SD card: clean partition table, create new partition, and format
    static Result<void> fixCard(DiskId diskId, const SdFixConfig& config,
                                SdProgressCallback progress = nullptr);

private:
    // Clean the disk by creating a fresh MBR or GPT
    static Result<void> cleanDisk(RawDiskHandle& disk, uint64_t diskSize);

    // Create a single partition spanning the entire disk
    static Result<void> createPartition(RawDiskHandle& disk, uint64_t diskSize,
                                         uint32_t sectorSize, FilesystemType fs);

    // Quick format the new partition
    static Result<void> formatPartition(DiskId diskId, FilesystemType fs,
                                         const std::wstring& label);

    // Force Windows to rescan the disk for new partitions
    static Result<void> rescanDisk(RawDiskHandle& disk);

    // Check if a disk looks like an SD card based on bus type, removability, size
    static bool looksLikeSdCard(const DiskInfo& disk);
};

} // namespace spw
