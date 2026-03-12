#pragma once

#include <cstdint>
#include <string>

namespace spw
{

// Sector-level addressing (LBA)
using SectorOffset = uint64_t;
using SectorCount = uint64_t;

// Byte-level sizes
using ByteCount = uint64_t;

// Identifiers
using DiskId = int;       // Physical drive index (0, 1, 2, ...)
using PartitionId = int;  // Partition index within a disk

// GUID as 16-byte array
struct Guid
{
    uint8_t data[16] = {};

    bool operator==(const Guid& other) const;
    bool operator!=(const Guid& other) const;
    bool isZero() const;
    std::string toString() const;
    static Guid fromString(const std::string& str);
    static Guid generate();
};

// Partition table type
enum class PartitionTableType
{
    Unknown,
    MBR,
    GPT,
    APM, // Apple Partition Map
};

// Filesystem types — comprehensive list including legacy systems
enum class FilesystemType
{
    Unknown,

    // Modern Windows
    NTFS,
    FAT32,
    FAT16,
    FAT12,
    ExFAT,
    ReFS,

    // Linux
    Ext2,
    Ext3,
    Ext4,
    Btrfs,
    XFS,
    ZFS,
    JFS,
    ReiserFS,
    Reiser4,

    // Apple
    HFSPlus,
    APFS,
    HFS,      // Classic Mac OS (legacy)
    MFS,      // Macintosh File System (1984)

    // Legacy / retro (1980s-1990s)
    FAT8,           // CP/M-86 era
    HPFS,           // OS/2 High Performance File System
    UFS,            // Unix File System (BSD)
    FFS,            // Berkeley Fast File System
    Minix,          // MINIX filesystem
    Xiafs,          // Linux early filesystem
    ADFS,           // Acorn Disc Filing System
    AfFS,           // Amiga Fast File System
    OFS,            // Amiga Old File System
    BFS_BeOS,       // BeOS File System
    QNX4,           // QNX4 filesystem
    QNX6,           // QNX6 filesystem
    SysV,           // System V filesystem
    Coherent,       // Coherent filesystem
    Xenix,          // Xenix filesystem
    VxFS,           // Veritas File System
    UDF,            // Universal Disk Format (optical media)
    ISO9660,        // CD-ROM filesystem
    RomFS,          // Read-only filesystem
    CramFS,         // Compressed ROM filesystem
    SquashFS,       // Compressed read-only filesystem
    VFAT,           // Virtual FAT (long filename extension)
    UMSDOS,         // Unix on MS-DOS filesystem

    // Network / special (read-only detection)
    NFS,
    SMB,
    SWAP_LINUX,
    SWAP_SOLARIS,

    // Raw / unformatted
    Raw,
    Unallocated,
};

// Disk interface types
enum class DiskInterfaceType
{
    Unknown,
    SATA,
    NVMe,
    USB,
    SCSI,
    SAS,
    IDE,
    MMC,      // SD cards, eMMC
    Firewire,
    Thunderbolt,
    Virtual,  // VHD, VHDX, etc.
};

// Access mode for opening raw disks or volumes
enum class DiskAccessMode
{
    ReadOnly,
    ReadWrite,
};

// Media types
enum class MediaType
{
    Unknown,
    HDD,
    SSD,
    NVMe,
    USBFlash,
    SDCard,
    CompactFlash,
    OpticalDrive,
    FloppyDisk,
    MemoryStick,
    Virtual,
};

} // namespace spw
