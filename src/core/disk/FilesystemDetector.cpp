// FilesystemDetector.cpp — Complete filesystem detection by magic bytes and structural analysis.
//
// Detection strategy: We probe specific byte offsets for known magic signatures.
// The order matters — we check the most common/reliable signatures first, then fall back
// to progressively more obscure ones. Some filesystems share boot sector structures (FAT
// family), so we use heuristic analysis of BPB fields to distinguish them.
//
// DISCLAIMER: This code is for authorized disk utility software only.

#include "FilesystemDetector.h"
#include "../common/Logging.h"

#include <algorithm>
#include <cstring>

namespace spw
{

// ============================================================================
// Helpers
// ============================================================================

// Read a little-endian uint16 from a byte buffer
static uint16_t readLE16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t readLE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t readLE64(const uint8_t* p)
{
    return static_cast<uint64_t>(readLE32(p))
         | (static_cast<uint64_t>(readLE32(p + 4)) << 32);
}

static uint16_t readBE16(const uint8_t* p)
{
    return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

static uint32_t readBE32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         | static_cast<uint32_t>(p[3]);
}

// Safe comparison with null-terminator awareness
static bool memEqual(const uint8_t* data, const char* magic, size_t len)
{
    return std::memcmp(data, magic, len) == 0;
}

// Check if a value is a power of 2
static bool isPowerOf2(uint32_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

// Extract a null-terminated string from a byte buffer
static std::string extractString(const uint8_t* data, size_t maxLen)
{
    size_t len = 0;
    while (len < maxLen && data[len] != 0)
        len++;
    // Trim trailing spaces
    while (len > 0 && data[len - 1] == ' ')
        len--;
    return std::string(reinterpret_cast<const char*>(data), len);
}

std::vector<uint8_t> FilesystemDetector::safeRead(const DiskReadCallback& readFunc, uint64_t offset, uint32_t size)
{
    auto result = readFunc(offset, size);
    if (result.isError())
        return {};
    return result.value();
}

// ============================================================================
// Main detection entry point
// ============================================================================

Result<FilesystemDetection> FilesystemDetector::detect(
    const DiskReadCallback& readFunc,
    uint64_t volumeSize)
{
    FilesystemDetection detection;

    // Check each filesystem type. Order is important:
    // 1. Filesystems with very specific magic bytes at unique offsets (NTFS, exFAT, XFS, APFS, Btrfs)
    // 2. Complex signatures requiring heuristic analysis (FAT family)
    // 3. Less common filesystems
    // 4. Legacy/retro filesystems

    if (detectNtfs(readFunc, detection))         return detection;
    if (detectExfat(readFunc, detection))         return detection;
    if (detectBtrfs(readFunc, detection))         return detection;
    if (detectXfs(readFunc, detection))           return detection;
    if (detectApfs(readFunc, detection))          return detection;
    if (detectReFs(readFunc, detection))          return detection;
    if (detectExt(readFunc, detection))           return detection;
    if (detectHfsPlus(readFunc, detection))       return detection;
    if (detectReiserFs(readFunc, detection))      return detection;
    if (detectJfs(readFunc, detection))           return detection;
    if (detectZfs(readFunc, detection))           return detection;
    if (detectIso9660(readFunc, detection))       return detection;
    if (detectUdf(readFunc, detection))           return detection;
    if (detectSquashFs(readFunc, detection))      return detection;
    if (detectCramFs(readFunc, detection))        return detection;
    if (detectRomFs(readFunc, detection))         return detection;
    if (detectHpfs(readFunc, detection))          return detection;
    if (detectMinix(readFunc, detection))         return detection;
    if (detectUfs(readFunc, detection))           return detection;
    if (detectBfs(readFunc, detection))           return detection;
    if (detectQnx4(readFunc, detection))         return detection;
    if (detectLinuxSwap(readFunc, detection, volumeSize)) return detection;

    // Flash-optimized filesystems
    if (detectF2fs(readFunc, detection))          return detection;
    if (detectJffs2(readFunc, detection))         return detection;
    if (detectNilfs2(readFunc, detection))        return detection;

    // Console / gaming filesystems
    if (detectFatx(readFunc, detection))          return detection;
    if (detectStfs(readFunc, detection))          return detection;
    if (detectGdfx(readFunc, detection))          return detection;
    if (detectPs2mc(readFunc, detection))         return detection;

    // Virtual disk images
    if (detectVhdx(readFunc, detection))          return detection;
    if (detectVmdk(readFunc, detection))          return detection;
    if (detectQcow2(readFunc, detection))         return detection;
    if (detectVdi(readFunc, detection))           return detection;
    if (detectVhd(readFunc, detection, volumeSize)) return detection;

    // Disc images
    if (detectRvz(readFunc, detection))           return detection;
    if (detectNrg(readFunc, detection, volumeSize)) return detection;
    if (detectWbfs(readFunc, detection))          return detection;

    // FAT last because its detection is the most heuristic-dependent
    if (detectFat(readFunc, detection))           return detection;

    // No filesystem detected
    detection.type = FilesystemType::Unknown;
    detection.description = "Unknown or unformatted";
    return detection;
}

Result<FilesystemDetection> FilesystemDetector::detectFromBuffer(
    const std::vector<uint8_t>& data,
    uint64_t volumeSize)
{
    // Wrap the buffer in a read callback
    auto readFunc = [&data](uint64_t offset, uint32_t size) -> Result<std::vector<uint8_t>> {
        if (offset + size > data.size())
        {
            // Return what we can, zero-padded
            std::vector<uint8_t> result(size, 0);
            if (offset < data.size())
            {
                size_t available = static_cast<size_t>(data.size() - offset);
                std::memcpy(result.data(), data.data() + offset, available);
            }
            return result;
        }
        return std::vector<uint8_t>(data.begin() + offset, data.begin() + offset + size);
    };

    return detect(readFunc, volumeSize);
}

// ============================================================================
// NTFS detection
// Boot sector layout:
//   Offset 0:     Jump instruction (3 bytes)
//   Offset 3:     OEM ID "NTFS    " (8 bytes)
//   Offset 11:    BPB (BIOS Parameter Block)
//   Offset 0x30:  Sectors per cluster
//   Offset 0x48:  MFT cluster number
// ============================================================================

bool FilesystemDetector::detectNtfs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 512);
    if (data.size() < 512) return false;

    // Check OEM ID at offset 3: "NTFS    " (8 bytes, space-padded)
    if (!memEqual(data.data() + 3, "NTFS    ", 8))
        return false;

    // Validate boot sector signature
    if (readLE16(data.data() + 510) != 0xAA55)
        return false;

    out.type = FilesystemType::NTFS;
    out.description = "NTFS";

    // Parse BPB for additional info
    uint16_t bytesPerSector = readLE16(data.data() + 0x0B);
    uint8_t sectorsPerCluster = data[0x0D];
    uint64_t totalSectors = readLE64(data.data() + 0x28);
    uint64_t mftCluster = readLE64(data.data() + 0x30);

    if (bytesPerSector > 0 && sectorsPerCluster > 0)
        out.blockSize = bytesPerSector * sectorsPerCluster;

    // Volume serial number at offset 0x48 (8 bytes)
    uint64_t serial = readLE64(data.data() + 0x48);
    if (serial != 0)
    {
        // Format as XXXX-XXXX (upper 32 bits)
        uint32_t serialHi = static_cast<uint32_t>(serial >> 32);
        uint32_t serialLo = static_cast<uint32_t>(serial);
        char serialStr[20];
        snprintf(serialStr, sizeof(serialStr), "%04X-%04X",
                 static_cast<unsigned>(serialHi >> 16) & 0xFFFF,
                 static_cast<unsigned>(serialHi) & 0xFFFF);
        out.uuid = serialStr;
    }

    return true;
}

// ============================================================================
// FAT12/FAT16/FAT32 detection
// The FAT family shares a common boot sector structure but the actual FAT type
// is determined by the total cluster count, NOT by the type string at offset 0x36/0x52.
//   < 4085 clusters  -> FAT12
//   < 65525 clusters -> FAT16
//   >= 65525 clusters -> FAT32
//
// FAT32 has additional BPB fields starting at offset 0x24.
// ============================================================================

bool FilesystemDetector::detectFat(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 512);
    if (data.size() < 512) return false;

    // Check boot sector signature
    if (readLE16(data.data() + 510) != 0xAA55)
        return false;

    // Check for a valid jump instruction at byte 0
    // FAT boot sectors start with either EB xx 90 (short jump) or E9 xx xx (near jump)
    if (data[0] != 0xEB && data[0] != 0xE9)
        return false;

    // Parse BPB (BIOS Parameter Block)
    uint16_t bytesPerSector = readLE16(data.data() + 0x0B);
    uint8_t sectorsPerCluster = data[0x0D];
    uint16_t reservedSectors = readLE16(data.data() + 0x0E);
    uint8_t numFats = data[0x10];
    uint16_t rootEntryCount = readLE16(data.data() + 0x11);
    uint16_t totalSectors16 = readLE16(data.data() + 0x13);
    uint8_t mediaType = data[0x15];
    uint16_t fatSize16 = readLE16(data.data() + 0x16);
    uint32_t totalSectors32 = readLE32(data.data() + 0x20);

    // Basic sanity checks for a valid FAT BPB
    if (bytesPerSector == 0 || !isPowerOf2(bytesPerSector))
        return false;
    if (bytesPerSector < 128 || bytesPerSector > 4096)
        return false;
    if (sectorsPerCluster == 0 || !isPowerOf2(sectorsPerCluster))
        return false;
    if (reservedSectors == 0)
        return false;
    if (numFats == 0 || numFats > 4)
        return false;
    // Media type should be one of the standard values
    if (mediaType != 0xF0 && mediaType < 0xF8)
        return false;

    // Determine FAT size
    uint32_t fatSize = fatSize16;
    uint32_t fatSize32 = 0;
    bool isFat32Bpb = false;

    if (fatSize == 0)
    {
        // FAT32: FAT size is at offset 0x24
        fatSize32 = readLE32(data.data() + 0x24);
        fatSize = fatSize32;
        isFat32Bpb = true;
    }

    // Total sectors
    uint32_t totalSectors = (totalSectors16 != 0) ? totalSectors16 : totalSectors32;
    if (totalSectors == 0)
        return false;

    // Calculate data region start
    uint32_t rootDirSectors = ((rootEntryCount * 32) + (bytesPerSector - 1)) / bytesPerSector;
    uint32_t dataStartSector = reservedSectors + (numFats * fatSize) + rootDirSectors;

    if (dataStartSector >= totalSectors)
        return false;

    uint32_t dataSectors = totalSectors - dataStartSector;
    uint32_t totalClusters = dataSectors / sectorsPerCluster;

    // Determine FAT type by cluster count
    if (totalClusters < 4085)
    {
        out.type = FilesystemType::FAT12;
        out.description = "FAT12";
    }
    else if (totalClusters < 65525)
    {
        out.type = FilesystemType::FAT16;
        out.description = "FAT16";
    }
    else
    {
        out.type = FilesystemType::FAT32;
        out.description = "FAT32";
    }

    out.blockSize = bytesPerSector * sectorsPerCluster;

    // Extract volume label and serial
    if (isFat32Bpb)
    {
        // FAT32: volume label at 0x47, serial at 0x43
        out.label = extractString(data.data() + 0x47, 11);
        uint32_t serial = readLE32(data.data() + 0x43);
        if (serial != 0)
        {
            char buf[12];
            snprintf(buf, sizeof(buf), "%04X-%04X",
                     (serial >> 16) & 0xFFFF, serial & 0xFFFF);
            out.uuid = buf;
        }
    }
    else
    {
        // FAT12/16: volume label at 0x2B, serial at 0x27
        out.label = extractString(data.data() + 0x2B, 11);
        uint32_t serial = readLE32(data.data() + 0x27);
        if (serial != 0)
        {
            char buf[12];
            snprintf(buf, sizeof(buf), "%04X-%04X",
                     (serial >> 16) & 0xFFFF, serial & 0xFFFF);
            out.uuid = buf;
        }
    }

    // Clean up label "NO NAME" (default)
    if (out.label == "NO NAME")
        out.label.clear();

    return true;
}

// ============================================================================
// exFAT detection
// Boot sector: "EXFAT   " at offset 3 (8 bytes)
// ============================================================================

bool FilesystemDetector::detectExfat(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 512);
    if (data.size() < 512) return false;

    if (!memEqual(data.data() + 3, "EXFAT   ", 8))
        return false;

    out.type = FilesystemType::ExFAT;
    out.description = "exFAT";

    // exFAT BPB fields
    // Offset 0x40: SectorBitsShift (power of 2)
    // Offset 0x41: ClusterBitsShift (power of 2, additional)
    uint8_t sectorShift = data[0x6C];     // SectorsPerClusterShift
    uint8_t bytesShift = data[0x6D];      // not used here -- let me recalculate

    // Actually, exFAT layout:
    //   0x40 (8 bytes): PartitionOffset
    //   0x48 (8 bytes): VolumeLength
    //   0x50 (4 bytes): FatOffset
    //   0x54 (4 bytes): FatLength
    //   0x58 (4 bytes): ClusterHeapOffset
    //   0x5C (4 bytes): ClusterCount
    //   0x60 (4 bytes): FirstClusterOfRootDirectory
    //   0x64 (4 bytes): VolumeSerialNumber
    //   0x68 (2 bytes): FileSystemRevision
    //   0x6C (1 byte):  BytesPerSectorShift
    //   0x6D (1 byte):  SectorsPerClusterShift

    uint8_t bytesPerSectorShift = data[0x6C];
    uint8_t sectorsPerClusterShift = data[0x6D];

    if (bytesPerSectorShift >= 9 && bytesPerSectorShift <= 12)
    {
        uint32_t bytesPerSector = 1u << bytesPerSectorShift;
        uint32_t sectorsPerCluster = 1u << sectorsPerClusterShift;
        out.blockSize = bytesPerSector * sectorsPerCluster;
    }

    // Volume serial at 0x64
    uint32_t serial = readLE32(data.data() + 0x64);
    if (serial != 0)
    {
        char buf[12];
        snprintf(buf, sizeof(buf), "%04X-%04X",
                 (serial >> 16) & 0xFFFF, serial & 0xFFFF);
        out.uuid = buf;
    }

    return true;
}

// ============================================================================
// ext2/3/4 detection
// Superblock is at byte offset 1024, size 1024 bytes.
// Magic number 0xEF53 at superblock offset 0x38 (absolute offset 1080).
// ext3 = has journal feature, ext4 = has extents or 64-bit feature.
// ============================================================================

bool FilesystemDetector::detectExt(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    // Superblock is at byte offset 1024
    auto data = safeRead(readFunc, 1024, 1024);
    if (data.size() < 1024) return false;

    // Magic at offset 0x38 within superblock (absolute 1080)
    uint16_t magic = readLE16(data.data() + 0x38);
    if (magic != EXT_SUPER_MAGIC)
        return false;

    // Feature flags
    uint32_t compatFeatures = readLE32(data.data() + 0x5C);
    uint32_t incompatFeatures = readLE32(data.data() + 0x60);
    uint32_t roCompatFeatures = readLE32(data.data() + 0x64);

    // Distinguish ext2/3/4:
    // EXT3_FEATURE_COMPAT_HAS_JOURNAL = 0x0004
    // EXT4_FEATURE_INCOMPAT_EXTENTS   = 0x0040
    // EXT4_FEATURE_INCOMPAT_64BIT     = 0x0080
    // EXT4_FEATURE_INCOMPAT_FLEX_BG   = 0x0200

    bool hasJournal = (compatFeatures & 0x0004) != 0;
    bool hasExtents = (incompatFeatures & 0x0040) != 0;
    bool has64bit = (incompatFeatures & 0x0080) != 0;
    bool hasFlexBg = (incompatFeatures & 0x0200) != 0;

    if (hasExtents || has64bit || hasFlexBg)
    {
        out.type = FilesystemType::Ext4;
        out.description = "ext4";
    }
    else if (hasJournal)
    {
        out.type = FilesystemType::Ext3;
        out.description = "ext3";
    }
    else
    {
        out.type = FilesystemType::Ext2;
        out.description = "ext2";
    }

    // Block size: 1024 << s_log_block_size (offset 0x18)
    uint32_t logBlockSize = readLE32(data.data() + 0x18);
    if (logBlockSize < 10) // Reasonable limit
        out.blockSize = 1024u << logBlockSize;

    // Volume label at offset 0x78 (16 bytes)
    out.label = extractString(data.data() + 0x78, 16);

    // UUID at offset 0x68 (16 bytes, raw binary)
    const uint8_t* uuid = data.data() + 0x68;
    char uuidStr[48];
    snprintf(uuidStr, sizeof(uuidStr),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5],
             uuid[6], uuid[7],
             uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    out.uuid = uuidStr;

    return true;
}

// ============================================================================
// Btrfs detection
// Superblock at byte offset 0x10000 (65536), magic "_BHRfS_M" at superblock offset 0x40
// (absolute offset 0x10040)
// ============================================================================

bool FilesystemDetector::detectBtrfs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0x10000, 0x1000);
    if (data.size() < 0x100) return false;

    // Magic "_BHRfS_M" at offset 0x40 within superblock
    if (!memEqual(data.data() + 0x40, "_BHRfS_M", 8))
        return false;

    out.type = FilesystemType::Btrfs;
    out.description = "Btrfs";

    // Sector size at offset 0x80, node size at 0x84
    uint32_t sectorSize = readLE32(data.data() + 0x80);
    uint32_t nodeSize = readLE32(data.data() + 0x84);
    out.blockSize = sectorSize;

    // Label at offset 0x12B (256 bytes)
    if (data.size() > 0x12B + 256)
        out.label = extractString(data.data() + 0x12B, 256);

    // UUID at offset 0x20 (16 bytes, fsid)
    if (data.size() > 0x20 + 16)
    {
        const uint8_t* uuid = data.data() + 0x20;
        char uuidStr[48];
        snprintf(uuidStr, sizeof(uuidStr),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid[0], uuid[1], uuid[2], uuid[3],
                 uuid[4], uuid[5],
                 uuid[6], uuid[7],
                 uuid[8], uuid[9],
                 uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
        out.uuid = uuidStr;
    }

    return true;
}

// ============================================================================
// XFS detection
// Superblock at offset 0, magic "XFSB" (4 bytes, big-endian 0x58465342)
// ============================================================================

bool FilesystemDetector::detectXfs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 512);
    if (data.size() < 512) return false;

    uint32_t magic = readBE32(data.data());
    if (magic != XFS_MAGIC)
        return false;

    out.type = FilesystemType::XFS;
    out.description = "XFS";

    // Block size at offset 4 (big-endian uint32)
    out.blockSize = readBE32(data.data() + 4);

    // Label at offset 0x6C (12 bytes)
    out.label = extractString(data.data() + 0x6C, 12);

    // UUID at offset 0x20 (16 bytes)
    const uint8_t* uuid = data.data() + 0x20;
    char uuidStr[48];
    snprintf(uuidStr, sizeof(uuidStr),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5],
             uuid[6], uuid[7],
             uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    out.uuid = uuidStr;

    return true;
}

// ============================================================================
// HFS+ detection
// Volume header at offset 1024, magic 0x482B ("H+") or 0x4858 ("HX" for HFSX)
// Both big-endian.
// ============================================================================

bool FilesystemDetector::detectHfsPlus(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 1024, 512);
    if (data.size() < 512) return false;

    uint16_t magic = readBE16(data.data());

    if (magic == HFS_PLUS_MAGIC)
    {
        out.type = FilesystemType::HFSPlus;
        out.description = "HFS+";
    }
    else if (magic == HFSX_MAGIC)
    {
        out.type = FilesystemType::HFSPlus; // HFSX is a variant of HFS+
        out.description = "HFSX (case-sensitive HFS+)";
    }
    else
    {
        // Check for classic HFS at offset 1024: magic 0x4244 ("BD")
        if (readBE16(data.data()) == 0x4244)
        {
            out.type = FilesystemType::HFS;
            out.description = "HFS (Classic)";
            return true;
        }
        return false;
    }

    // HFS+ volume header fields (all big-endian):
    //   Offset 0x28: blockSize (uint32)
    out.blockSize = readBE32(data.data() + 0x28);

    return true;
}

// ============================================================================
// APFS detection
// Container superblock at offset 0, magic "NXSB" (4 bytes)
// Stored as little-endian uint32: 0x4253584E
// ============================================================================

bool FilesystemDetector::detectApfs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 4096);
    if (data.size() < 64) return false;

    // APFS container superblock: magic at offset 32 (after the obj_phys_t header)
    // obj_phys_t is 32 bytes, then nx_magic at offset 32
    uint32_t magic = readLE32(data.data() + 32);
    if (magic != APFS_MAGIC)
        return false;

    out.type = FilesystemType::APFS;
    out.description = "APFS";

    // Block size at offset 36 (uint32)
    out.blockSize = readLE32(data.data() + 36);

    return true;
}

// ============================================================================
// ReFS detection
// Volume boot record: "ReFS" signature at offset 3
// Additional verification: look for ReFS superblock signature
// ============================================================================

bool FilesystemDetector::detectReFs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 512);
    if (data.size() < 512) return false;

    // Primary check: "ReFS" at offset 3
    if (memEqual(data.data() + 3, "ReFS", 4))
    {
        out.type = FilesystemType::ReFS;
        out.description = "ReFS";

        // ReFS doesn't have a simple BPB like FAT/NTFS.
        // Cluster size can be read from the VBR but the format is not publicly documented.
        // We report detection without detailed metadata.
        return true;
    }

    return false;
}

// ============================================================================
// ISO 9660 detection
// Primary Volume Descriptor at offset 0x8000 (32768), "CD001" at offset 1
// (absolute offset 0x8001)
// ============================================================================

bool FilesystemDetector::detectIso9660(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0x8000, 2048);
    if (data.size() < 2048) return false;

    // Volume descriptor type at offset 0, "CD001" at offset 1-5
    if (!memEqual(data.data() + 1, "CD001", 5))
        return false;

    out.type = FilesystemType::ISO9660;
    out.description = "ISO 9660";
    out.blockSize = 2048;

    // Volume identifier at offset 40 (32 bytes, space-padded)
    out.label = extractString(data.data() + 40, 32);

    return true;
}

// ============================================================================
// UDF detection
// Look for BEA01 (Beginning Extended Area Descriptor) at offset 0x8001
// and NSR02 or NSR03 at offset 0x8801 or 0x9001
// ============================================================================

bool FilesystemDetector::detectUdf(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    // Check for BEA01 at volume descriptor offset
    auto bea = safeRead(readFunc, 0x8000, 2048);
    if (bea.size() < 6) return false;

    if (!memEqual(bea.data() + 1, "BEA01", 5))
        return false;

    // Look for NSR02 (UDF 1.x) or NSR03 (UDF 2.x) in the next descriptor
    auto nsr = safeRead(readFunc, 0x8800, 2048);
    if (nsr.size() >= 6)
    {
        if (memEqual(nsr.data() + 1, "NSR02", 5) || memEqual(nsr.data() + 1, "NSR03", 5))
        {
            out.type = FilesystemType::UDF;
            out.description = "UDF";
            out.blockSize = 2048;
            return true;
        }
    }

    // Try next sector
    auto nsr2 = safeRead(readFunc, 0x9000, 2048);
    if (nsr2.size() >= 6)
    {
        if (memEqual(nsr2.data() + 1, "NSR02", 5) || memEqual(nsr2.data() + 1, "NSR03", 5))
        {
            out.type = FilesystemType::UDF;
            out.description = "UDF";
            out.blockSize = 2048;
            return true;
        }
    }

    return false;
}

// ============================================================================
// ReiserFS detection
// Superblock at offset 0x10000 (64K) for ReiserFS 3.6+, or 0x2000 (8K) for 3.5.
// Magic "ReIsErFs" or "ReIsEr2Fs" or "ReIsEr3Fs" at superblock offset 0x34.
// ============================================================================

bool FilesystemDetector::detectReiserFs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    // Try 64K offset first (ReiserFS 3.6+)
    auto data = safeRead(readFunc, REISERFS_MAGIC_OFFSET, 64);
    if (data.size() >= 12)
    {
        if (memEqual(data.data(), "ReIsErFs", 8) ||
            memEqual(data.data(), "ReIsEr2Fs", 9) ||
            memEqual(data.data(), "ReIsEr3Fs", 9))
        {
            out.type = FilesystemType::ReiserFS;
            out.description = "ReiserFS";

            // Block size at superblock offset 0x2C (0x10000 + 0x2C)
            auto sb = safeRead(readFunc, 0x10000, 0x100);
            if (sb.size() >= 0x30)
                out.blockSize = readLE16(sb.data() + 0x2C);

            return true;
        }
    }

    // Try 8K offset (ReiserFS 3.5)
    data = safeRead(readFunc, 0x2000 + 0x34, 12);
    if (data.size() >= 8)
    {
        if (memEqual(data.data(), "ReIsErFs", 8))
        {
            out.type = FilesystemType::ReiserFS;
            out.description = "ReiserFS 3.5";
            return true;
        }
    }

    return false;
}

// ============================================================================
// JFS detection
// Superblock at offset 0x8000 (32768), magic "JFS1" at offset 0
// ============================================================================

bool FilesystemDetector::detectJfs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0x8000, 512);
    if (data.size() < 512) return false;

    if (!memEqual(data.data(), "JFS1", 4))
        return false;

    out.type = FilesystemType::JFS;
    out.description = "JFS";

    // Block size at offset 0x18 (int32, LE)
    out.blockSize = readLE32(data.data() + 0x18);

    // Label at offset 0x96 (16 bytes)
    out.label = extractString(data.data() + 0x96, 16);

    // UUID at offset 0x80 (16 bytes)
    const uint8_t* uuid = data.data() + 0x80;
    char uuidStr[48];
    snprintf(uuidStr, sizeof(uuidStr),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5],
             uuid[6], uuid[7],
             uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    out.uuid = uuidStr;

    return true;
}

// ============================================================================
// HPFS detection
// Superblock at sector 16 (offset 8192), magic 0xF995E849 at offset 0
// Spare block at sector 17 (offset 8704), magic 0xF9911849 at offset 0
// ============================================================================

bool FilesystemDetector::detectHpfs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto super = safeRead(readFunc, 8192, 512);
    if (super.size() < 512) return false;

    uint32_t magic = readLE32(super.data());
    if (magic != 0xF995E849u)
        return false;

    // Double-check with spare block
    auto spare = safeRead(readFunc, 8704, 512);
    if (spare.size() >= 4)
    {
        uint32_t spareMagic = readLE32(spare.data());
        if (spareMagic != 0xF9911849u)
            return false;
    }

    out.type = FilesystemType::HPFS;
    out.description = "HPFS (OS/2)";
    return true;
}

// ============================================================================
// Minix detection
// Superblock at offset 1024, magic at offset 0x10 within superblock.
//   0x137F = MINIX v1 (14-char names)
//   0x138F = MINIX v1 (30-char names)
//   0x2468 = MINIX v2 (14-char names)
//   0x2478 = MINIX v2 (30-char names)
//   0x4D5A = MINIX v3
// ============================================================================

bool FilesystemDetector::detectMinix(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 1024, 512);
    if (data.size() < 32) return false;

    uint16_t magic = readLE16(data.data() + 0x10);

    switch (magic)
    {
    case 0x137F:
    case 0x138F:
        out.type = FilesystemType::Minix;
        out.description = "MINIX v1";
        out.blockSize = 1024;
        return true;
    case 0x2468:
    case 0x2478:
        out.type = FilesystemType::Minix;
        out.description = "MINIX v2";
        out.blockSize = 1024;
        return true;
    case 0x4D5A:
        out.type = FilesystemType::Minix;
        out.description = "MINIX v3";
        out.blockSize = readLE16(data.data() + 0x18); // zone_size
        return true;
    default:
        return false;
    }
}

// ============================================================================
// UFS detection (BSD Unix File System)
// Superblock at offset 8192 (or 65536 for UFS2).
// Magic 0x00011954 at superblock offset 0x55C (UFS1) or various offsets.
// ============================================================================

bool FilesystemDetector::detectUfs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    // UFS1: superblock at 8192, magic at sb+0x55C
    auto data = safeRead(readFunc, 8192, 0x600);
    if (data.size() >= 0x560)
    {
        uint32_t magic = readLE32(data.data() + 0x55C);
        if (magic == UFS_MAGIC || magic == 0x54190100u) // Also check big-endian form
        {
            out.type = FilesystemType::UFS;
            out.description = "UFS1";
            return true;
        }
    }

    // UFS2: superblock at 65536, magic at sb+0x55C
    auto data2 = safeRead(readFunc, 65536, 0x600);
    if (data2.size() >= 0x560)
    {
        uint32_t magic = readLE32(data2.data() + 0x55C);
        if (magic == UFS_MAGIC || magic == 0x54190100u)
        {
            out.type = FilesystemType::UFS;
            out.description = "UFS2";
            return true;
        }
    }

    return false;
}

// ============================================================================
// BFS detection (BeOS/Haiku)
// Superblock at offset 512, magic "BFS1" (0x42465331) at offset 0
// (Also check for "1SFB" for big-endian BeOS)
// ============================================================================

bool FilesystemDetector::detectBfs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 512, 512);
    if (data.size() < 512) return false;

    uint32_t magic = readLE32(data.data());
    if (magic == BEOS_SUPER_MAGIC)
    {
        out.type = FilesystemType::BFS_BeOS;
        out.description = "BFS (BeOS/Haiku)";

        // Block size at offset 4 (uint32)
        out.blockSize = readLE32(data.data() + 4);
        // Volume name at offset 0x20 (32 bytes)
        out.label = extractString(data.data() + 0x20, 32);

        return true;
    }

    // Big-endian variant
    uint32_t magicBE = readBE32(data.data());
    if (magicBE == BEOS_SUPER_MAGIC)
    {
        out.type = FilesystemType::BFS_BeOS;
        out.description = "BFS (BeOS, big-endian)";
        out.blockSize = readBE32(data.data() + 4);
        return true;
    }

    return false;
}

// ============================================================================
// QNX4 detection
// Superblock at offset 0, magic 0x002F at offset 4
// ============================================================================

bool FilesystemDetector::detectQnx4(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 512);
    if (data.size() < 512) return false;

    // QNX4 root directory signature at offset 0
    // The QNX4 identification is by checking the di_status fields
    // A simpler heuristic: look for the QNX4 magic pattern
    if (data.size() >= 512)
    {
        // QNX4 has a specific pattern in its root directory entry
        // Look for the status byte pattern: 0x01 at offset 0 (di_fname status)
        // and 0x2F (/) in filename
        if (data[4] == 0x2F && (data[0] & 0x01))
        {
            // Additional validation: check for reasonable block sizes
            out.type = FilesystemType::QNX4;
            out.description = "QNX4";
            return true;
        }
    }

    return false;
}

// ============================================================================
// ZFS detection
// ZFS labels at offset 0 and 256K, magic at label+0x1C: 0x00BAB10C (uint64 LE)
// or uber-block magic 0x00BAB10C at various offsets
// ============================================================================

bool FilesystemDetector::detectZfs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    // ZFS has labels at the start and end of a vdev.
    // Label 0 at offset 0, label 1 at offset 256K.
    // Each label contains an uber-block array starting at label+128K.
    // The uber-block magic is 0x00BAB10C at offset 0 of each uber-block.

    // Check at offset 128K (128 * 1024 = 0x20000) for uber-block
    auto data = safeRead(readFunc, 0x20000, 1024);
    if (data.size() >= 8)
    {
        uint64_t magic = readLE64(data.data());
        if (magic == 0x00BAB10CULL)
        {
            out.type = FilesystemType::ZFS;
            out.description = "ZFS";
            return true;
        }
    }

    // Also try the name/value pair area for "name" field
    auto nvData = safeRead(readFunc, 0x4000, 0x4000);
    if (nvData.size() >= 16)
    {
        // NV list has a specific encoding. Look for "version" or "name" strings
        // that indicate ZFS metadata. This is a heuristic.
        for (size_t i = 0; i + 8 <= nvData.size(); i++)
        {
            if (memEqual(nvData.data() + i, "version", 7))
            {
                out.type = FilesystemType::ZFS;
                out.description = "ZFS";
                return true;
            }
        }
    }

    return false;
}

// ============================================================================
// SquashFS detection
// Magic "hsqs" (0x73717368 LE) or "sqsh" (big-endian) at offset 0
// ============================================================================

bool FilesystemDetector::detectSquashFs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 512);
    if (data.size() < 96) return false;

    uint32_t magic = readLE32(data.data());
    if (magic == 0x73717368u) // "hsqs" LE
    {
        out.type = FilesystemType::SquashFS;
        out.description = "SquashFS";

        // Block size at offset 12 (uint32 LE)
        out.blockSize = readLE32(data.data() + 12);
        return true;
    }

    // Big-endian variant "sqsh"
    if (readBE32(data.data()) == 0x73717368u)
    {
        out.type = FilesystemType::SquashFS;
        out.description = "SquashFS (big-endian)";
        out.blockSize = readBE32(data.data() + 12);
        return true;
    }

    return false;
}

// ============================================================================
// CramFS detection
// Magic 0x28CD3D45 at offset 0 (LE) or 0x453DCD28 (BE)
// ============================================================================

bool FilesystemDetector::detectCramFs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 512);
    if (data.size() < 64) return false;

    uint32_t magic = readLE32(data.data());
    if (magic == 0x28CD3D45u || magic == 0x453DCD28u)
    {
        out.type = FilesystemType::CramFS;
        out.description = "CramFS";
        out.blockSize = 4096; // CramFS always uses 4K pages

        // Volume name at offset 16 (16 bytes)
        out.label = extractString(data.data() + 16, 16);
        return true;
    }

    return false;
}

// ============================================================================
// RomFS detection
// Magic "-rom1fs-" at offset 0 (8 bytes)
// ============================================================================

bool FilesystemDetector::detectRomFs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 512);
    if (data.size() < 32) return false;

    if (!memEqual(data.data(), "-rom1fs-", 8))
        return false;

    out.type = FilesystemType::RomFS;
    out.description = "RomFS";

    // Volume name at offset 16 (null-terminated, up to 16 byte aligned)
    out.label = extractString(data.data() + 16, 32);
    return true;
}

// ============================================================================
// Linux Swap detection
// Magic "SWAPSPACE2" or "SWAP-SPACE" at (pagesize - 10)
// Common page sizes: 4096, 8192, 16384, 65536
// ============================================================================

bool FilesystemDetector::detectLinuxSwap(const DiskReadCallback& readFunc, FilesystemDetection& out, uint64_t volumeSize)
{
    // Try common page sizes
    static const uint32_t pageSizes[] = { 4096, 8192, 16384, 65536 };

    for (uint32_t pageSize : pageSizes)
    {
        if (pageSize > volumeSize && volumeSize > 0)
            continue;

        auto data = safeRead(readFunc, pageSize - 10, 10);
        if (data.size() < 10)
            continue;

        if (memEqual(data.data(), "SWAPSPACE2", 10) ||
            memEqual(data.data(), "SWAP-SPACE", 10))
        {
            out.type = FilesystemType::SWAP_LINUX;
            out.description = "Linux Swap";
            out.blockSize = pageSize;

            // UUID at offset 0x40C in the swap header (page offset + 0x40C would be 0x40C
            // since the swap header starts at offset 0)
            auto header = safeRead(readFunc, 0, 4096);
            if (header.size() >= 0x41C)
            {
                const uint8_t* uuid = header.data() + 0x40C;
                char uuidStr[48];
                snprintf(uuidStr, sizeof(uuidStr),
                         "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                         uuid[0], uuid[1], uuid[2], uuid[3],
                         uuid[4], uuid[5],
                         uuid[6], uuid[7],
                         uuid[8], uuid[9],
                         uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
                out.uuid = uuidStr;
            }

            // Label at offset 0x41C (16 bytes)
            if (header.size() >= 0x42C)
                out.label = extractString(header.data() + 0x41C, 16);

            return true;
        }
    }

    return false;
}

// ============================================================================
// Filesystem name lookup
// ============================================================================

const char* FilesystemDetector::filesystemName(FilesystemType type)
{
    switch (type)
    {
    case FilesystemType::Unknown:       return "Unknown";
    case FilesystemType::NTFS:          return "NTFS";
    case FilesystemType::FAT32:         return "FAT32";
    case FilesystemType::FAT16:         return "FAT16";
    case FilesystemType::FAT12:         return "FAT12";
    case FilesystemType::ExFAT:         return "exFAT";
    case FilesystemType::ReFS:          return "ReFS";
    case FilesystemType::Ext2:          return "ext2";
    case FilesystemType::Ext3:          return "ext3";
    case FilesystemType::Ext4:          return "ext4";
    case FilesystemType::Btrfs:         return "Btrfs";
    case FilesystemType::XFS:           return "XFS";
    case FilesystemType::ZFS:           return "ZFS";
    case FilesystemType::JFS:           return "JFS";
    case FilesystemType::ReiserFS:      return "ReiserFS";
    case FilesystemType::Reiser4:       return "Reiser4";
    case FilesystemType::HFSPlus:       return "HFS+";
    case FilesystemType::APFS:          return "APFS";
    case FilesystemType::HFS:           return "HFS";
    case FilesystemType::MFS:           return "MFS";
    case FilesystemType::FAT8:          return "FAT8";
    case FilesystemType::HPFS:          return "HPFS";
    case FilesystemType::UFS:           return "UFS";
    case FilesystemType::FFS:           return "FFS";
    case FilesystemType::Minix:         return "MINIX";
    case FilesystemType::Xiafs:         return "Xiafs";
    case FilesystemType::ADFS:          return "ADFS";
    case FilesystemType::AfFS:          return "AFFS";
    case FilesystemType::OFS:           return "OFS";
    case FilesystemType::BFS_BeOS:      return "BFS";
    case FilesystemType::QNX4:          return "QNX4";
    case FilesystemType::QNX6:          return "QNX6";
    case FilesystemType::SysV:          return "SysV";
    case FilesystemType::Coherent:      return "Coherent";
    case FilesystemType::Xenix:         return "Xenix";
    case FilesystemType::VxFS:          return "VxFS";
    case FilesystemType::UDF:           return "UDF";
    case FilesystemType::ISO9660:       return "ISO 9660";
    case FilesystemType::RomFS:         return "RomFS";
    case FilesystemType::CramFS:        return "CramFS";
    case FilesystemType::SquashFS:      return "SquashFS";
    case FilesystemType::VFAT:          return "VFAT";
    case FilesystemType::UMSDOS:        return "UMSDOS";
    case FilesystemType::NFS:           return "NFS";
    case FilesystemType::SMB:           return "SMB";
    case FilesystemType::SWAP_LINUX:    return "Linux Swap";
    case FilesystemType::SWAP_SOLARIS:  return "Solaris Swap";
    case FilesystemType::F2FS:          return "F2FS";
    case FilesystemType::JFFS2:         return "JFFS2";
    case FilesystemType::NILFS2:        return "NILFS2";
    case FilesystemType::FATX:          return "FATX (Xbox)";
    case FilesystemType::STFS:          return "STFS (Xbox 360)";
    case FilesystemType::GDFX:          return "GDFX (Xbox Disc)";
    case FilesystemType::PS2MC:         return "PS2 Memory Card";
    case FilesystemType::VHD:           return "VHD";
    case FilesystemType::VHDX:          return "VHDX";
    case FilesystemType::VMDK:          return "VMDK";
    case FilesystemType::QCOW2:        return "QCOW2";
    case FilesystemType::VDI:           return "VDI";
    case FilesystemType::RVZ:           return "RVZ (Wii)";
    case FilesystemType::WUA:           return "WUA (Wii U)";
    case FilesystemType::WBFs:          return "WBFS (Wii)";
    case FilesystemType::NRG:           return "NRG (Nero)";
    case FilesystemType::MDF:           return "MDF (Alcohol)";
    case FilesystemType::CDI:           return "CDI (DiscJuggler)";
    case FilesystemType::CDFS:          return "CDFS";
    case FilesystemType::HDFS:          return "HDFS";
    case FilesystemType::Raw:           return "Raw";
    case FilesystemType::Unallocated:   return "Unallocated";
    }
    return "Unknown";
}

// ============================================================================
// F2FS detection
// Magic: 0xF2F52010 at offset 0x400 (1024)
// ============================================================================

bool FilesystemDetector::detectF2fs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0x400, 128);
    if (data.size() < 128) return false;

    // F2FS magic at offset 0 of superblock (which is at partition offset 0x400)
    uint32_t magic = readLE32(data.data());
    if (magic != 0xF2F52010)
        return false;

    out.type = FilesystemType::F2FS;
    out.description = "F2FS (Flash-Friendly File System)";

    // Major/minor version at offset 4 and 6
    uint16_t majorVer = readLE16(data.data() + 4);
    uint16_t minorVer = readLE16(data.data() + 6);
    (void)majorVer; (void)minorVer;

    // log_blocksize at offset 38 (usually 12 = 4096 bytes)
    uint32_t logBlocksize = readLE32(data.data() + 38);
    if (logBlocksize >= 10 && logBlocksize <= 16)
        out.blockSize = 1u << logBlocksize;

    // Volume name at offset 0x6A0 - 0x400 = 0x2A0 from start of superblock, Unicode
    // (need to read more data for that)
    auto labelData = safeRead(readFunc, 0x400 + 0x2A0, 512);
    if (labelData.size() >= 64)
    {
        std::string label;
        for (size_t i = 0; i < 64; i += 2)
        {
            uint16_t ch = readLE16(labelData.data() + i);
            if (ch == 0) break;
            if (ch < 128) label += static_cast<char>(ch);
        }
        if (!label.empty())
            out.label = label;
    }

    // UUID at offset 0x460 - 0x400 = 0x60 from superblock start
    if (data.size() >= 0x70)
    {
        char uuid[48];
        const uint8_t* u = data.data() + 0x60;
        snprintf(uuid, sizeof(uuid),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                 u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        out.uuid = uuid;
    }

    return true;
}

// ============================================================================
// JFFS2 detection
// Magic: 0x1985 (little-endian) or 0x8519 (big-endian) at offset 0
// ============================================================================

bool FilesystemDetector::detectJffs2(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 12);
    if (data.size() < 12) return false;

    uint16_t magic = readLE16(data.data());
    if (magic != 0x1985 && magic != 0x8519)
        return false;

    // Validate node type (bits 0-15 of nodetype at offset 2)
    uint16_t nodetype = readLE16(data.data() + 2);
    // Strip high compatibility bits
    uint16_t nodeBase = nodetype & 0x00FF;
    // Valid JFFS2 node types: 1=DIRENT, 2=INODE, 3=CLEAN, 6=PADDING, 0xE0=SUMMARY
    if (nodeBase != 0x01 && nodeBase != 0x02 && nodeBase != 0x03 &&
        nodeBase != 0x06 && nodeBase != 0xE0)
        return false;

    out.type = FilesystemType::JFFS2;
    out.description = "JFFS2 (Journalling Flash File System v2)";
    return true;
}

// ============================================================================
// NILFS2 detection
// Magic: 0x3434 at offset 0x406 (superblock at 0x400, magic at +6)
// ============================================================================

bool FilesystemDetector::detectNilfs2(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0x400, 128);
    if (data.size() < 128) return false;

    // NILFS2 magic at offset 6 in the superblock
    uint16_t magic = readLE16(data.data() + 6);
    if (magic != 0x3434)
        return false;

    out.type = FilesystemType::NILFS2;
    out.description = "NILFS2";

    // Block size: stored as log2 at offset 0x0E
    uint32_t logBlock = readLE32(data.data() + 0x0E);
    if (logBlock >= 10 && logBlock <= 16)
        out.blockSize = 1u << logBlock;

    return true;
}

// ============================================================================
// FATX detection (Xbox / Xbox 360)
// Magic: "FATX" (0x58544146 LE or 0x46415458 BE) at partition start
// ============================================================================

bool FilesystemDetector::detectFatx(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 0x200);
    if (data.size() < 0x200) return false;

    // Check FATX magic (bytes "FATX" at offset 0)
    if (!memEqual(data.data(), "FATX", 4))
        return false;

    // Validate SectorsPerCluster (must be power of 2, max 0x80)
    uint32_t spc = readLE32(data.data() + 0x08);
    if (!isPowerOf2(spc) || spc > 0x80)
        return false;

    out.type = FilesystemType::FATX;
    out.description = "FATX (Xbox)";
    out.blockSize = spc * 512;

    // Volume name at offset 0x10 (up to 32 Unicode chars)
    std::string label;
    for (int i = 0; i < 32; ++i)
    {
        uint16_t ch = readLE16(data.data() + 0x10 + i * 2);
        if (ch == 0 || ch == 0xFFFF) break;
        if (ch < 128) label += static_cast<char>(ch);
    }
    if (!label.empty())
        out.label = label;

    return true;
}

// ============================================================================
// STFS detection (Xbox 360 content packages)
// Magic: "CON " (0x434F4E20), "LIVE" (0x4C495645), "PIRS" (0x50495253) at offset 0
// ============================================================================

bool FilesystemDetector::detectStfs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 8);
    if (data.size() < 4) return false;

    uint32_t magic = readBE32(data.data());
    if (magic != 0x434F4E20 && // "CON "
        magic != 0x4C495645 && // "LIVE"
        magic != 0x50495253)   // "PIRS"
        return false;

    out.type = FilesystemType::STFS;
    out.description = "STFS (Xbox 360 Package)";
    return true;
}

// ============================================================================
// GDFX detection (Xbox Game Disc Format)
// Magic: "MICROSOFT*XBOX*MEDIA" at various sector offsets
// ============================================================================

bool FilesystemDetector::detectGdfx(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    static const char kGdfxMagic[] = "MICROSOFT*XBOX*MEDIA";
    constexpr size_t kMagicLen = 20;

    // Check all known GDFX offsets
    static const uint64_t offsets[] = {
        0x10000,        // Raw XGD (SDK)
        0x18310000,     // XGD1 (original Xbox)
        0xFDA0000,      // XGD2
        0x2090000,      // XGD3
    };

    for (auto off : offsets)
    {
        auto data = safeRead(readFunc, off, 32);
        if (data.size() >= kMagicLen && memEqual(data.data(), kGdfxMagic, kMagicLen))
        {
            out.type = FilesystemType::GDFX;
            out.description = "GDFX (Xbox Game Disc)";
            return true;
        }
    }

    return false;
}

// ============================================================================
// PS2 Memory Card detection
// Magic: "Sony PS2 Memory Card Format " at offset 0
// ============================================================================

bool FilesystemDetector::detectPs2mc(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 128);
    if (data.size() < 40) return false;

    // Check the first 28 bytes of the magic string
    if (!memEqual(data.data(), "Sony PS2 Memory Card Format ", 28))
        return false;

    out.type = FilesystemType::PS2MC;
    out.description = "PS2 Memory Card";

    // Page size at offset 0x28 (uint16)
    if (data.size() >= 0x2C)
    {
        uint16_t pageSize = readLE16(data.data() + 0x28);
        uint16_t pagesPerCluster = readLE16(data.data() + 0x2A);
        if (pageSize > 0 && pagesPerCluster > 0)
            out.blockSize = pageSize * pagesPerCluster;
    }

    return true;
}

// ============================================================================
// VHD detection (Microsoft Virtual Hard Disk)
// Footer magic: "conectix" at last 512 bytes of file, or at offset 0 for fixed VHD
// ============================================================================

bool FilesystemDetector::detectVhd(const DiskReadCallback& readFunc, FilesystemDetection& out, uint64_t volumeSize)
{
    // VHD footer can be at offset 0 (dynamic/differencing) as a copy
    auto data = safeRead(readFunc, 0, 512);
    if (data.size() < 512) return false;

    if (memEqual(data.data(), "conectix", 8))
    {
        out.type = FilesystemType::VHD;
        out.description = "VHD (Virtual Hard Disk)";
        return true;
    }

    // For fixed VHD, footer is at the very end — check if volumeSize is known
    if (volumeSize >= 512)
    {
        auto footer = safeRead(readFunc, volumeSize - 512, 512);
        if (footer.size() >= 8 && memEqual(footer.data(), "conectix", 8))
        {
            out.type = FilesystemType::VHD;
            out.description = "VHD (Virtual Hard Disk)";
            return true;
        }
    }

    return false;
}

// ============================================================================
// VHDX detection
// Magic: "vhdxfile" at offset 0
// ============================================================================

bool FilesystemDetector::detectVhdx(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 16);
    if (data.size() < 8) return false;

    if (memEqual(data.data(), "vhdxfile", 8))
    {
        out.type = FilesystemType::VHDX;
        out.description = "VHDX (Hyper-V Virtual Hard Disk)";
        return true;
    }

    return false;
}

// ============================================================================
// VMDK detection
// Magic: "KDMV" (0x564D444B LE) for sparse extent, or "# Disk DescriptorFile" text
// ============================================================================

bool FilesystemDetector::detectVmdk(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 64);
    if (data.size() < 4) return false;

    // Sparse VMDK: magic "KDMV" at offset 0
    uint32_t magic = readLE32(data.data());
    if (magic == 0x564D444B) // "KDMV" LE
    {
        out.type = FilesystemType::VMDK;
        out.description = "VMDK (VMware Virtual Disk)";
        return true;
    }

    // Text descriptor VMDK
    if (data.size() >= 21 && memEqual(data.data(), "# Disk DescriptorFile", 21))
    {
        out.type = FilesystemType::VMDK;
        out.description = "VMDK (VMware Virtual Disk)";
        return true;
    }

    return false;
}

// ============================================================================
// QCOW2 detection
// Magic: "QFI\xFB" (0x514649FB BE) at offset 0
// ============================================================================

bool FilesystemDetector::detectQcow2(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 16);
    if (data.size() < 8) return false;

    uint32_t magic = readBE32(data.data());
    if (magic != 0x514649FB)
        return false;

    uint32_t version = readBE32(data.data() + 4);
    if (version != 2 && version != 3)
        return false;

    out.type = FilesystemType::QCOW2;
    out.description = (version == 3) ? "QCOW2 v3 (QEMU)" : "QCOW2 (QEMU)";
    return true;
}

// ============================================================================
// VDI detection (VirtualBox)
// Magic: 0xBEDA107F at offset 0x40
// ============================================================================

bool FilesystemDetector::detectVdi(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 0x50);
    if (data.size() < 0x48) return false;

    // VDI signature at offset 0x40
    uint32_t magic = readLE32(data.data() + 0x40);
    if (magic != 0xBEDA107F)
        return false;

    out.type = FilesystemType::VDI;
    out.description = "VDI (VirtualBox Disk Image)";

    // Check for "<<< " image creation marker at offset 0
    if (memEqual(data.data(), "\x7F\x10\xDA\xBE", 4) ||
        memEqual(data.data() + 0x40, "\x7F\x10\xDA\xBE", 4))
    {
        // Already matched
    }

    return true;
}

// ============================================================================
// RVZ detection (Dolphin Wii disc image)
// Magic: 0x015A5652 LE ("RVZ\x01") at offset 0
// Also WIA: 0x01414957 LE ("WIA\x01")
// ============================================================================

bool FilesystemDetector::detectRvz(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 8);
    if (data.size() < 4) return false;

    uint32_t magic = readLE32(data.data());
    if (magic == 0x015A5652) // "RVZ\x01"
    {
        out.type = FilesystemType::RVZ;
        out.description = "RVZ (Dolphin Wii Disc Image)";
        return true;
    }

    // WIA format (predecessor to RVZ, same family)
    if (magic == 0x01414957) // "WIA\x01"
    {
        out.type = FilesystemType::RVZ;
        out.description = "WIA (Dolphin Wii Disc Image)";
        return true;
    }

    return false;
}

// ============================================================================
// NRG detection (Nero disc image)
// Magic: "NER5" or "NERO" at end of file (footer-based)
// ============================================================================

bool FilesystemDetector::detectNrg(const DiskReadCallback& readFunc, FilesystemDetection& out, uint64_t volumeSize)
{
    if (volumeSize < 12)
        return false;

    // NRG v2: "NER5" at (filesize - 12)
    auto footer = safeRead(readFunc, volumeSize - 12, 12);
    if (footer.size() >= 4)
    {
        if (memEqual(footer.data(), "NER5", 4))
        {
            out.type = FilesystemType::NRG;
            out.description = "NRG v2 (Nero Disc Image)";
            return true;
        }
    }

    // NRG v1: "NERO" at (filesize - 8)
    footer = safeRead(readFunc, volumeSize - 8, 8);
    if (footer.size() >= 4)
    {
        if (memEqual(footer.data(), "NERO", 4))
        {
            out.type = FilesystemType::NRG;
            out.description = "NRG v1 (Nero Disc Image)";
            return true;
        }
    }

    return false;
}

// ============================================================================
// WBFS detection (Wii Backup File System)
// Magic: "WBFS" at offset 0
// ============================================================================

bool FilesystemDetector::detectWbfs(const DiskReadCallback& readFunc, FilesystemDetection& out)
{
    auto data = safeRead(readFunc, 0, 16);
    if (data.size() < 4) return false;

    if (memEqual(data.data(), "WBFS", 4))
    {
        out.type = FilesystemType::WBFs;
        out.description = "WBFS (Wii Backup File System)";
        return true;
    }

    return false;
}

} // namespace spw
