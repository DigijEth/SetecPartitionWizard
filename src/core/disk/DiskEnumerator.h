#pragma once

// DiskEnumerator — Enumerates physical disks, partitions, and volumes on Windows.
// Uses SetupAPI for physical disk discovery, WMI for disk-partition-volume mapping,
// and FindFirstVolumeW/FindNextVolumeW for volume enumeration.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace spw
{

// Information about a physical disk
struct DiskInfo
{
    DiskId id = -1;                                          // Physical drive index
    std::wstring model;                                      // e.g. L"Samsung SSD 970 EVO Plus"
    std::wstring serialNumber;
    std::wstring firmwareRevision;
    uint64_t sizeBytes = 0;
    uint32_t sectorSize = 512;
    DiskInterfaceType interfaceType = DiskInterfaceType::Unknown;
    MediaType mediaType = MediaType::Unknown;
    bool isRemovable = false;
    PartitionTableType partitionTableType = PartitionTableType::Unknown;
    std::wstring devicePath;                                 // e.g. L"\\.\PhysicalDrive0"
};

// Information about a partition on a disk
struct PartitionInfo
{
    DiskId diskId = -1;
    PartitionId index = -1;
    uint64_t offsetBytes = 0;
    uint64_t sizeBytes = 0;
    FilesystemType filesystemType = FilesystemType::Unknown;
    std::wstring label;
    wchar_t driveLetter = L'\0';                             // L'\0' if no drive letter
    std::wstring volumeGuidPath;                             // e.g. L"\\?\Volume{GUID}\"
    bool isActive = false;
    bool isBootable = false;

    // MBR-specific
    uint8_t mbrType = 0;

    // GPT-specific
    Guid gptTypeGuid;
    Guid gptPartitionGuid;
};

// Information about a mounted volume
struct VolumeInfo
{
    std::wstring guidPath;                                   // e.g. L"\\?\Volume{GUID}\"
    std::vector<std::wstring> mountPoints;                   // Drive letters and folder mounts
    std::wstring filesystemLabel;
    std::wstring filesystemName;                             // e.g. L"NTFS"
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
};

// Complete system disk snapshot
struct SystemDiskSnapshot
{
    std::vector<DiskInfo> disks;
    std::vector<PartitionInfo> partitions;
    std::vector<VolumeInfo> volumes;
};

namespace DiskEnumerator
{

// Enumerate all physical disks. Uses SetupAPI (SetupDiGetClassDevs) with
// GUID_DEVINTERFACE_DISK and falls back to iterating PhysicalDrive0..31.
Result<std::vector<DiskInfo>> enumerateDisks();

// Enumerate all volumes using FindFirstVolumeW/FindNextVolumeW and
// GetVolumePathNamesForVolumeNameW for mount points.
Result<std::vector<VolumeInfo>> enumerateVolumes();

// Use WMI to build the full disk -> partition -> volume mapping.
// Queries Win32_DiskDrive, Win32_DiskDriveToDiskPartition, Win32_LogicalDiskToPartition.
Result<std::vector<PartitionInfo>> enumeratePartitionsWmi();

// Full system snapshot combining all three enumerations.
Result<SystemDiskSnapshot> getSystemSnapshot();

// Get info for a single disk by index.
Result<DiskInfo> getDiskInfo(DiskId diskIndex);

// Helper: classify interface type from a WMI InterfaceType string
DiskInterfaceType classifyInterfaceType(const std::wstring& wmiInterfaceType);

// Helper: classify media type from WMI MediaType string and interface hints
MediaType classifyMediaType(const std::wstring& wmiMediaType, DiskInterfaceType ifType);

// Helper: convert WMI filesystem name string to FilesystemType enum
FilesystemType classifyFilesystem(const std::wstring& fsName);

} // namespace DiskEnumerator
} // namespace spw
