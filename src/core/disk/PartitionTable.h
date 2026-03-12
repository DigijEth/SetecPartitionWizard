#pragma once

// PartitionTable — Abstract base class and concrete MBR/GPT/APM partition table implementations.
// Parses on-disk structures, validates integrity, and supports read/write operations.
//
// Reference specifications:
//   MBR: "Master Boot Record" — de facto standard, 512-byte sector 0
//   GPT: UEFI Specification, Chapter 5 — GUID Partition Table
//   APM: Apple Technote 1350 — Apple Partition Map
//
// DISCLAIMER: This code is for authorized disk utility software only.

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../common/Constants.h"
#include "DiskGeometry.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace spw
{

// ============================================================================
// Read callback: abstracts reading raw bytes from disk, file, or buffer.
// Parameters: (byteOffset, byteCount) -> raw data
// ============================================================================
using DiskReadCallback = std::function<Result<std::vector<uint8_t>>(uint64_t offset, uint32_t size)>;

// ============================================================================
// On-disk MBR partition entry (16 bytes, packed)
// Offsets relative to entry start:
//   0x00 [1]  Status / boot indicator (0x80 = active, 0x00 = inactive)
//   0x01 [3]  CHS of first sector
//   0x04 [1]  Partition type byte
//   0x05 [3]  CHS of last sector
//   0x08 [4]  LBA of first sector (little-endian)
//   0x0C [4]  Sector count (little-endian)
// ============================================================================
#pragma pack(push, 1)
struct MbrPartitionEntryRaw
{
    uint8_t  status;
    uint8_t  chsFirst[3];
    uint8_t  type;
    uint8_t  chsLast[3];
    uint32_t lbaStart;
    uint32_t sectorCount;
};
static_assert(sizeof(MbrPartitionEntryRaw) == 16, "MBR partition entry must be 16 bytes");

struct MbrSectorRaw
{
    uint8_t             bootCode[446];
    MbrPartitionEntryRaw entries[4];
    uint16_t            signature; // Must be 0xAA55
};
static_assert(sizeof(MbrSectorRaw) == 512, "MBR sector must be 512 bytes");

// On-disk GPT header (92 bytes at LBA 1)
struct GptHeaderRaw
{
    uint64_t signature;           // "EFI PART" = 0x5452415020494645
    uint32_t revision;            // Typically 0x00010000
    uint32_t headerSize;          // Usually 92
    uint32_t headerCrc32;         // CRC32 of header (with this field zeroed)
    uint32_t reserved;            // Must be zero
    uint64_t myLba;               // LBA of this header
    uint64_t alternateLba;        // LBA of alternate header
    uint64_t firstUsableLba;
    uint64_t lastUsableLba;
    uint8_t  diskGuid[16];
    uint64_t partitionEntryLba;   // LBA of partition entry array
    uint32_t partitionEntryCount; // Number of entries (usually 128)
    uint32_t partitionEntrySize;  // Size of each entry (usually 128)
    uint32_t partitionEntryCrc32; // CRC32 of entire entry array
};
static_assert(sizeof(GptHeaderRaw) == 92, "GPT header must be 92 bytes");

// On-disk GPT partition entry (128 bytes)
struct GptPartitionEntryRaw
{
    uint8_t  typeGuid[16];
    uint8_t  uniqueGuid[16];
    uint64_t startLba;
    uint64_t endLba;            // Inclusive
    uint64_t attributes;
    uint16_t name[36];          // UTF-16LE, null-terminated
};
static_assert(sizeof(GptPartitionEntryRaw) == 128, "GPT partition entry must be 128 bytes");

// On-disk APM Driver Descriptor Map (block 0)
struct ApmDdmRaw
{
    uint16_t signature;       // 0x4552 "ER"
    uint16_t blockSize;
    uint32_t blockCount;
    uint16_t deviceType;
    uint16_t deviceId;
    uint32_t driverData;
    uint16_t driverCount;
    uint8_t  reserved[486];   // Pad to 512 (or blockSize)
};

// On-disk APM partition entry (512 bytes per entry)
struct ApmPartitionEntryRaw
{
    uint16_t signature;       // 0x504D "PM"
    uint16_t reserved1;
    uint32_t mapEntries;      // Total number of partition map entries
    uint32_t pBlockStart;     // Physical block start
    uint32_t pBlockCount;     // Physical block count
    char     name[32];        // Null-terminated partition name
    char     type[32];        // Null-terminated type string (e.g. "Apple_HFS")
    uint32_t lBlockStart;     // Logical block start (within partition)
    uint32_t lBlockCount;     // Logical block count
    uint32_t flags;
    uint32_t bootBlockStart;
    uint32_t bootBlockCount;
    uint32_t bootLoadAddr;
    uint32_t reserved2;
    uint32_t bootEntryAddr;
    uint32_t reserved3;
    uint32_t bootChecksum;
    char     processor[16];
    uint8_t  padding[376];    // Pad to 512
};
#pragma pack(pop)

// ============================================================================
// Parsed partition entry — common structure across all table types
// ============================================================================
struct PartitionEntry
{
    int index = -1;                         // 0-based index in partition table

    SectorOffset startLba = 0;
    SectorCount  sectorCount = 0;
    uint32_t     sectorSize = SECTOR_SIZE_512; // Context: sector size of disk

    // MBR fields
    uint8_t mbrType = 0;                    // MBR type byte (0x07, 0x0C, etc.)
    bool    isActive = false;               // MBR bootable flag
    bool    isExtended = false;             // True if this is an extended container
    bool    isLogical = false;              // True if inside extended partition
    CHSAddress chsStart = {};
    CHSAddress chsEnd = {};

    // GPT fields
    Guid    typeGuid;                       // Partition type GUID
    Guid    uniqueGuid;                     // Unique partition GUID
    uint64_t gptAttributes = 0;
    std::string gptName;                    // UTF-8 name from GPT entry

    // APM fields
    std::string apmName;                    // APM partition name
    std::string apmType;                    // APM type string ("Apple_HFS", etc.)

    // Derived / cached
    FilesystemType detectedFs = FilesystemType::Unknown;
    std::string    label;                   // Filesystem label if detected

    // Convenience
    uint64_t startByte() const { return startLba * sectorSize; }
    uint64_t sizeBytes() const { return sectorCount * sectorSize; }
    SectorOffset endLba() const { return (sectorCount > 0) ? (startLba + sectorCount - 1) : startLba; }
};

// ============================================================================
// Parameters for creating a new partition
// ============================================================================
struct PartitionParams
{
    SectorOffset startLba = 0;
    SectorCount  sectorCount = 0;

    // MBR: type byte
    uint8_t mbrType = 0x07; // Default: NTFS/HPFS/exFAT

    // GPT: type GUID, name
    Guid typeGuid;
    std::string gptName;

    // Flags
    bool isActive = false;
    bool isLogical = false; // MBR: create inside extended partition
};

// ============================================================================
// Well-known MBR partition type bytes
// ============================================================================
namespace MbrTypes
{
    constexpr uint8_t Empty           = 0x00;
    constexpr uint8_t FAT12           = 0x01;
    constexpr uint8_t FAT16_Small     = 0x04; // < 32 MiB
    constexpr uint8_t Extended        = 0x05;
    constexpr uint8_t FAT16_Large     = 0x06; // >= 32 MiB
    constexpr uint8_t NTFS_HPFS       = 0x07;
    constexpr uint8_t FAT32_CHS       = 0x0B;
    constexpr uint8_t FAT32_LBA       = 0x0C;
    constexpr uint8_t FAT16_LBA       = 0x0E;
    constexpr uint8_t Extended_LBA    = 0x0F;
    constexpr uint8_t HiddenFAT32     = 0x1B;
    constexpr uint8_t HiddenFAT32_LBA = 0x1C;
    constexpr uint8_t DynDisk         = 0x42;
    constexpr uint8_t LinuxSwap       = 0x82;
    constexpr uint8_t LinuxNative     = 0x83;
    constexpr uint8_t LinuxExtended   = 0x85;
    constexpr uint8_t LinuxLVM        = 0x8E;
    constexpr uint8_t FreeBSD         = 0xA5;
    constexpr uint8_t OpenBSD         = 0xA6;
    constexpr uint8_t NetBSD          = 0xA9;
    constexpr uint8_t HFS_APM         = 0xAF;
    constexpr uint8_t GPT_Protective  = 0xEE;
    constexpr uint8_t EFI_System      = 0xEF;
    constexpr uint8_t LinuxRaid       = 0xFD;

    // Returns a human-readable name for an MBR type byte
    const char* typeName(uint8_t type);

    // Returns true if this type represents an extended/container partition
    bool isExtendedType(uint8_t type);
}

// ============================================================================
// Well-known GPT partition type GUIDs
// ============================================================================
namespace GptTypes
{
    // Microsoft
    Guid microsoftBasicData();    // EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
    Guid microsoftReserved();     // E3C9E316-0B5C-4DB8-817D-F92DF00215AE
    Guid efiSystem();             // C12A7328-F81F-11D2-BA4B-00A0C93EC93B
    Guid microsoftLdmMetadata();  // 5808C8AA-7E8F-42E0-85D2-E1E90434CFB3
    Guid microsoftLdmData();      // AF9B60A0-1431-4F62-BC68-3311714A69AD
    Guid microsoftRecovery();     // DE94BBA4-06D1-4D40-A16A-BFD50179D6AC

    // Linux
    Guid linuxFilesystem();       // 0FC63DAF-8483-4772-8E79-3D69D8477DE4
    Guid linuxSwap();             // 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F
    Guid linuxHome();             // 933AC7E1-2EB4-4F13-B844-0E14E2AEF915
    Guid linuxLvm();              // E6D6D379-F507-44C2-A23C-238F2A3DF928
    Guid linuxRaid();             // A19D880F-05FC-4D3B-A006-743F0F84911E

    // Apple
    Guid appleHfsPlus();          // 48465300-0000-11AA-AA11-00306543ECAC
    Guid appleApfs();             // 7C3457EF-0000-11AA-AA11-00306543ECAC
    Guid appleBoot();             // 426F6F74-0000-11AA-AA11-00306543ECAC

    // BSD
    Guid freebsdUfs();            // 516E7CB6-6ECF-11D6-8FF8-00022D09712B
    Guid freebsdSwap();           // 516E7CB5-6ECF-11D6-8FF8-00022D09712B
    Guid freebsdZfs();            // 516E7CBA-6ECF-11D6-8FF8-00022D09712B

    // Returns a human-readable name for a GPT type GUID
    std::string typeName(const Guid& guid);
}

// ============================================================================
// Abstract partition table interface
// ============================================================================
class PartitionTable
{
public:
    virtual ~PartitionTable() = default;

    // What kind of partition table is this?
    virtual PartitionTableType type() const = 0;

    // Get all parsed partition entries
    virtual std::vector<PartitionEntry> partitions() const = 0;

    // Modification operations
    virtual Result<void> addPartition(const PartitionParams& params) = 0;
    virtual Result<void> deletePartition(int index) = 0;
    virtual Result<void> resizePartition(int index, SectorOffset newStart, SectorCount newSize) = 0;

    // Serialize the entire partition table to bytes for writing back to disk
    virtual Result<std::vector<uint8_t>> serialize() const = 0;

    // Parse a partition table from a read callback.
    // Automatically detects MBR vs GPT (GPT protective MBR -> GPT).
    // APM is detected by the DDM signature at block 0.
    static Result<std::unique_ptr<PartitionTable>> parse(
        const DiskReadCallback& readFunc,
        uint64_t diskSizeBytes,
        uint32_t sectorSize = SECTOR_SIZE_512);

    // Create a brand new empty partition table
    static std::unique_ptr<PartitionTable> createNew(
        PartitionTableType type,
        uint64_t diskSizeBytes,
        uint32_t sectorSize = SECTOR_SIZE_512,
        const Guid& diskGuid = {});

protected:
    uint64_t m_diskSizeBytes = 0;
    uint32_t m_sectorSize = SECTOR_SIZE_512;
};

// ============================================================================
// MBR partition table
// ============================================================================
class MbrPartitionTable : public PartitionTable
{
public:
    MbrPartitionTable();

    PartitionTableType type() const override { return PartitionTableType::MBR; }
    std::vector<PartitionEntry> partitions() const override;
    Result<void> addPartition(const PartitionParams& params) override;
    Result<void> deletePartition(int index) override;
    Result<void> resizePartition(int index, SectorOffset newStart, SectorCount newSize) override;
    Result<std::vector<uint8_t>> serialize() const override;

    // Parse from raw sector data (reads MBR + walks EBR chain)
    Result<void> parse(const DiskReadCallback& readFunc);

    // Access to boot code for boot repair scenarios
    const std::array<uint8_t, 446>& bootCode() const { return m_bootCode; }
    void setBootCode(const std::array<uint8_t, 446>& code) { m_bootCode = code; }

    // Set active (bootable) partition. Pass -1 to clear.
    Result<void> setActivePartition(int index);

    // MBR disk signature (bytes 440-443)
    uint32_t diskSignature() const { return m_diskSignature; }
    void setDiskSignature(uint32_t sig) { m_diskSignature = sig; }

    // Does this MBR contain a GPT protective entry?
    bool hasGptProtective() const;

private:
    // Walk the extended partition EBR chain
    Result<void> walkExtendedChain(const DiskReadCallback& readFunc, SectorOffset extStart, SectorOffset extSize);

    // Find the extended partition (container), or -1
    int findExtendedIndex() const;

    // Check if a proposed region overlaps existing partitions
    bool overlapsExisting(SectorOffset start, SectorCount count, int excludeIndex = -1) const;

    std::array<uint8_t, 446> m_bootCode = {};
    uint32_t m_diskSignature = 0;
    uint16_t m_reserved = 0; // bytes 444-445

    // Primary entries (up to 4). Logical entries follow.
    std::vector<PartitionEntry> m_entries;
};

// ============================================================================
// GPT partition table
// ============================================================================
class GptPartitionTable : public PartitionTable
{
public:
    GptPartitionTable();

    PartitionTableType type() const override { return PartitionTableType::GPT; }
    std::vector<PartitionEntry> partitions() const override;
    Result<void> addPartition(const PartitionParams& params) override;
    Result<void> deletePartition(int index) override;
    Result<void> resizePartition(int index, SectorOffset newStart, SectorCount newSize) override;
    Result<std::vector<uint8_t>> serialize() const override;

    // Parse from read callback (reads protective MBR, primary GPT header + entries)
    Result<void> parse(const DiskReadCallback& readFunc);

    // Disk GUID
    Guid diskGuid() const { return m_diskGuid; }
    void setDiskGuid(const Guid& guid) { m_diskGuid = guid; }

    // Header revision
    uint32_t revision() const { return m_revision; }

    // Usable LBA range
    SectorOffset firstUsableLba() const { return m_firstUsableLba; }
    SectorOffset lastUsableLba() const { return m_lastUsableLba; }

    // Validate CRC32 of header and entry array
    Result<void> validateCrcs(const DiskReadCallback& readFunc) const;

    // Read backup GPT from end of disk
    Result<void> parseBackup(const DiskReadCallback& readFunc);

private:
    // Parse the entry array from raw bytes
    Result<void> parseEntries(const std::vector<uint8_t>& entryData);

    // Check for overlapping partitions
    bool overlapsExisting(SectorOffset start, SectorOffset end, int excludeIndex = -1) const;

    // Protective MBR bytes (preserved for serialization)
    std::array<uint8_t, 512> m_protectiveMbr = {};

    Guid     m_diskGuid;
    uint32_t m_revision = 0x00010000;
    uint64_t m_firstUsableLba = 34;
    uint64_t m_lastUsableLba = 0;
    uint64_t m_alternateLba = 0;
    uint32_t m_entryCount = GPT_MAX_PARTITIONS;
    uint32_t m_entrySize = GPT_ENTRY_SIZE;

    std::vector<PartitionEntry> m_entries;
};

// ============================================================================
// APM partition table (read-only)
// ============================================================================
class ApmPartitionTable : public PartitionTable
{
public:
    ApmPartitionTable();

    PartitionTableType type() const override { return PartitionTableType::APM; }
    std::vector<PartitionEntry> partitions() const override;

    // APM is read-only in this implementation
    Result<void> addPartition(const PartitionParams& params) override;
    Result<void> deletePartition(int index) override;
    Result<void> resizePartition(int index, SectorOffset newStart, SectorCount newSize) override;
    Result<std::vector<uint8_t>> serialize() const override;

    // Parse from read callback
    Result<void> parse(const DiskReadCallback& readFunc);

    // APM block size (from DDM, usually 512)
    uint32_t blockSize() const { return m_blockSize; }

private:
    uint32_t m_blockSize = 512;
    uint32_t m_blockCount = 0;
    std::vector<PartitionEntry> m_entries;
};

// ============================================================================
// CRC32 utility (for GPT header/entry validation)
// Uses the standard CRC-32/ISO-HDLC polynomial (0xEDB88320 reflected)
// ============================================================================
uint32_t crc32(const uint8_t* data, size_t length);
uint32_t crc32(const std::vector<uint8_t>& data);

} // namespace spw
