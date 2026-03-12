#pragma once

// PartitionOperations — Concrete operation classes for partition management.
//
// Each operation properly locks volumes, dismounts, updates partition tables,
// and notifies the kernel of changes via IOCTL_DISK_UPDATE_PROPERTIES.
//
// DISCLAIMER: This code is for authorized disk utility software only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "Operation.h"
#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../common/Constants.h"
#include "../disk/RawDiskHandle.h"
#include "../disk/VolumeHandle.h"
#include "../disk/PartitionTable.h"
#include "../filesystem/FormatEngine.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QString>

namespace spw
{

// ============================================================================
// CreatePartitionOp — Create a new partition in unallocated space
//
// Steps:
//   1. Open the physical disk
//   2. Read current partition table
//   3. Add new partition entry
//   4. Write updated partition table
//   5. Notify kernel (IOCTL_DISK_UPDATE_PROPERTIES)
//   6. Optionally format the new partition
//
// Undo: Delete the created partition entry
// ============================================================================
class CreatePartitionOp : public Operation
{
public:
    struct Params
    {
        DiskId diskId = -1;
        SectorOffset startLba = 0;
        SectorCount sectorCount = 0;
        uint32_t sectorSize = SECTOR_SIZE_512;

        // MBR specific
        uint8_t mbrType = MbrTypes::NTFS_HPFS;
        bool isActive = false;
        bool isLogical = false; // Create inside extended partition

        // GPT specific
        Guid typeGuid;         // Empty = Microsoft Basic Data
        std::string gptName;

        // Optional: format after creation
        bool formatAfter = false;
        FormatOptions formatOptions;
    };

    explicit CreatePartitionOp(const Params& params);

    Type type() const override { return Type::CreatePartition; }
    QString description() const override;
    Result<void> execute(ProgressCallback progress) override;
    Result<void> undo() override;
    bool canUndo() const override { return m_createdIndex >= 0; }

private:
    Params m_params;
    int m_createdIndex = -1; // Index of created partition (for undo)
};

// ============================================================================
// DeletePartitionOp — Delete a partition
//
// Steps:
//   1. Lock and dismount the volume (if mounted)
//   2. Optionally wipe first sectors (prevent accidental recognition)
//   3. Read partition table
//   4. Delete the partition entry
//   5. Write updated partition table
//   6. Notify kernel
//
// Undo: Re-create the partition entry (data may be gone if wiped)
// ============================================================================
class DeletePartitionOp : public Operation
{
public:
    struct Params
    {
        DiskId diskId = -1;
        int partitionIndex = -1;
        uint32_t sectorSize = SECTOR_SIZE_512;
        wchar_t driveLetter = 0; // If mounted, for dismount
        bool wipeFirstSectors = true; // Zero first 4K to prevent FS detection
    };

    explicit DeletePartitionOp(const Params& params);

    Type type() const override { return Type::DeletePartition; }
    QString description() const override;
    Result<void> execute(ProgressCallback progress) override;
    Result<void> undo() override;
    bool canUndo() const override { return m_savedEntry.has_value(); }

private:
    Params m_params;
    std::optional<PartitionEntry> m_savedEntry; // Saved for undo
};

// ============================================================================
// ResizePartitionOp — Resize (and optionally move) a partition
//
// Steps:
//   1. Lock and dismount
//   2. Read partition table
//   3. Validate new size/position (no overlap, within disk bounds)
//   4. If shrinking: resize filesystem first, then shrink partition entry
//   5. If growing: grow partition entry, then resize filesystem
//   6. If moving: copy data, update entry
//   7. Write updated partition table
//   8. Notify kernel
//
// Undo: Restore original partition entry (filesystem resize may not be reversible)
// ============================================================================
class ResizePartitionOp : public Operation
{
public:
    struct Params
    {
        DiskId diskId = -1;
        int partitionIndex = -1;
        uint32_t sectorSize = SECTOR_SIZE_512;
        wchar_t driveLetter = 0;

        SectorOffset newStartLba = 0;
        SectorCount newSectorCount = 0;
    };

    explicit ResizePartitionOp(const Params& params);

    Type type() const override { return Type::ResizePartition; }
    QString description() const override;
    Result<void> execute(ProgressCallback progress) override;
    Result<void> undo() override;
    bool canUndo() const override { return m_savedEntry.has_value(); }

private:
    Params m_params;
    std::optional<PartitionEntry> m_savedEntry;
};

// ============================================================================
// FormatPartitionOp — Format an existing partition to a new filesystem
//
// Steps:
//   1. Identify partition (drive letter or raw disk + offset)
//   2. Delegate to FormatEngine
//   3. Notify kernel
//
// Undo: Not generally undoable (original data is destroyed)
// ============================================================================
class FormatPartitionOp : public Operation
{
public:
    struct Params
    {
        FormatTarget target;
        FormatOptions options;
        DiskId diskId = -1;
        int partitionIndex = -1;
    };

    explicit FormatPartitionOp(const Params& params);

    Type type() const override { return Type::FormatPartition; }
    QString description() const override;
    Result<void> execute(ProgressCallback progress) override;
    bool canUndo() const override { return false; }

private:
    Params m_params;
};

// ============================================================================
// SetLabelOp — Change the volume label
//
// For NTFS/FAT/exFAT: SetVolumeLabelW()
// For ext2/3/4: Direct superblock write
//
// Undo: Restore the previous label
// ============================================================================
class SetLabelOp : public Operation
{
public:
    struct Params
    {
        wchar_t driveLetter = 0;
        std::string newLabel;

        // For raw access (ext filesystems without drive letter)
        DiskId diskId = -1;
        int partitionIndex = -1;
        uint64_t partitionOffsetBytes = 0;
        uint32_t sectorSize = SECTOR_SIZE_512;
        FilesystemType fsType = FilesystemType::Unknown;
    };

    explicit SetLabelOp(const Params& params);

    Type type() const override { return Type::SetLabel; }
    QString description() const override;
    Result<void> execute(ProgressCallback progress) override;
    Result<void> undo() override;
    bool canUndo() const override { return m_oldLabelSaved; }

private:
    Params m_params;
    std::string m_oldLabel;
    bool m_oldLabelSaved = false;
};

// ============================================================================
// SetFlagsOp — Set partition flags (active/bootable, hidden, etc.)
//
// MBR: Set/clear the active (bootable) flag (0x80 status byte)
// GPT: Modify partition attributes (system, hidden, read-only, etc.)
//
// Undo: Restore previous flags
// ============================================================================
class SetFlagsOp : public Operation
{
public:
    struct Params
    {
        DiskId diskId = -1;
        int partitionIndex = -1;
        uint32_t sectorSize = SECTOR_SIZE_512;

        // MBR flags
        std::optional<bool> setActive;    // Set/clear bootable flag

        // GPT attributes
        std::optional<uint64_t> gptAttributes; // Full attribute mask
    };

    explicit SetFlagsOp(const Params& params);

    Type type() const override { return Type::SetFlags; }
    QString description() const override;
    Result<void> execute(ProgressCallback progress) override;
    Result<void> undo() override;
    bool canUndo() const override { return m_savedEntry.has_value(); }

private:
    Params m_params;
    std::optional<PartitionEntry> m_savedEntry;
};

// ============================================================================
// CheckFilesystemOp — Run filesystem consistency check
//
// NTFS/FAT/exFAT: Run chkdsk.exe
// ext2/3/4: Direct superblock state check (limited without e2fsck binary)
//
// Undo: Not applicable (read-only check) or not reversible (repair)
// ============================================================================
class CheckFilesystemOp : public Operation
{
public:
    struct Params
    {
        wchar_t driveLetter = 0;
        bool repair = false;       // /F flag for chkdsk
        bool badSectorScan = false; // /R flag for chkdsk

        // For raw access (non-Windows filesystems)
        DiskId diskId = -1;
        int partitionIndex = -1;
        uint64_t partitionOffsetBytes = 0;
        uint32_t sectorSize = SECTOR_SIZE_512;
        FilesystemType fsType = FilesystemType::Unknown;
    };

    explicit CheckFilesystemOp(const Params& params);

    Type type() const override { return Type::CheckFilesystem; }
    QString description() const override;
    Result<void> execute(ProgressCallback progress) override;
    bool canUndo() const override { return false; }

private:
    Params m_params;
};

// ============================================================================
// Utility functions shared by operations
// ============================================================================
namespace OperationUtils
{
    // Read the partition table from a disk
    Result<std::unique_ptr<PartitionTable>> readPartitionTable(
        DiskId diskId, uint32_t sectorSize);

    // Write a partition table back to disk
    Result<void> writePartitionTable(
        DiskId diskId, const PartitionTable& table, uint32_t sectorSize);

    // Notify the OS kernel that partition geometry changed
    Result<void> notifyKernel(DiskId diskId);

    // Lock and dismount a volume by drive letter
    Result<VolumeHandle> lockAndDismountVolume(wchar_t driveLetter);

    // Format size in bytes to human-readable string
    QString formatSize(uint64_t bytes);
}

} // namespace spw
