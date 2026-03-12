#pragma once

// RawDiskHandle — RAII wrapper for raw physical disk access via \\.\PhysicalDriveN.
// All operations return Result<T> so callers must handle errors explicitly.
// DISCLAIMER: This code is for authorized disk utility software only.

#include <windows.h>
#include <winioctl.h>

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace spw
{

// Parsed disk geometry returned from IOCTL_DISK_GET_DRIVE_GEOMETRY_EX
struct DiskGeometryInfo
{
    uint64_t totalBytes = 0;
    uint32_t bytesPerSector = 0;
    uint32_t sectorsPerTrack = 0;
    uint32_t tracksPerCylinder = 0;
    uint64_t cylinders = 0;
    MEDIA_TYPE mediaType = Unknown;
};

// Partition entry from IOCTL_DISK_GET_DRIVE_LAYOUT_EX
struct DriveLayoutPartition
{
    uint32_t partitionNumber = 0;
    uint64_t startingOffset = 0;
    uint64_t partitionLength = 0;
    bool rewritePartition = false;
    bool isRecognized = false;

    // MBR-specific
    uint8_t mbrPartitionType = 0;
    bool mbrBootIndicator = false;

    // GPT-specific
    Guid gptPartitionType;
    Guid gptPartitionId;
    uint64_t gptAttributes = 0;
    std::wstring gptName;
};

// Full drive layout
struct DriveLayoutInfo
{
    PartitionTableType partitionStyle = PartitionTableType::Unknown;
    uint32_t partitionCount = 0;

    // MBR-specific
    uint32_t mbrSignature = 0;

    // GPT-specific
    Guid gptDiskId;

    std::vector<DriveLayoutPartition> partitions;
};

class RawDiskHandle
{
public:
    RawDiskHandle() = default;
    ~RawDiskHandle();

    // Non-copyable, movable
    RawDiskHandle(const RawDiskHandle&) = delete;
    RawDiskHandle& operator=(const RawDiskHandle&) = delete;
    RawDiskHandle(RawDiskHandle&& other) noexcept;
    RawDiskHandle& operator=(RawDiskHandle&& other) noexcept;

    // Open \\.\PhysicalDriveN
    static Result<RawDiskHandle> open(DiskId diskIndex, DiskAccessMode mode);

    // Open by explicit device path (e.g. "\\.\PhysicalDrive0")
    static Result<RawDiskHandle> openPath(const std::wstring& devicePath, DiskAccessMode mode);

    // Returns true if the handle is valid
    bool isValid() const;

    // Close the handle (also called by destructor)
    void close();

    // Read sectors starting at the given LBA. Returns the data read.
    Result<std::vector<uint8_t>> readSectors(SectorOffset lba, SectorCount count, uint32_t sectorSize) const;

    // Write sectors at the given LBA. Buffer size must be a multiple of sectorSize.
    Result<void> writeSectors(SectorOffset lba, const uint8_t* data, SectorCount count, uint32_t sectorSize) const;

    // Get disk geometry
    Result<DiskGeometryInfo> getGeometry() const;

    // Get drive layout (partition table)
    Result<DriveLayoutInfo> getDriveLayout() const;

    // Lock a volume on this disk. volumeLetter is e.g. L'C'.
    // This opens \\.\X: internally and locks it.
    static Result<HANDLE> lockVolume(wchar_t volumeLetter);

    // Unlock a previously locked volume handle
    static Result<void> unlockVolume(HANDLE volumeHandle);

    // Dismount a volume by its letter
    static Result<void> dismountVolume(wchar_t volumeLetter);

    // Flush disk write buffers
    Result<void> flushBuffers() const;

    // Raw Win32 handle accessor (for advanced use)
    HANDLE nativeHandle() const { return m_handle; }
    DiskId diskId() const { return m_diskId; }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    DiskId m_diskId = -1;
    DiskAccessMode m_accessMode = DiskAccessMode::ReadOnly;
};

} // namespace spw
