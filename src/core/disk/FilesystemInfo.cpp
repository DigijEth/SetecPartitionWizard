// FilesystemInfo.cpp — Reads detailed filesystem metadata from on-disk structures.
//
// After FilesystemDetector identifies the filesystem type, this module reads the
// relevant superblock/BPB/volume header to extract label, UUID, sizes, feature flags,
// and other metadata. Each filesystem stores this information at different offsets
// in different formats — this is the single place that knows all those layouts.
//
// DISCLAIMER: This code is for authorized disk utility software only.

#include "FilesystemInfo.h"
#include "../common/Logging.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace spw
{

// ============================================================================
// Endian helpers (duplicated from FilesystemDetector.cpp for self-containment;
// in a larger project these would be in a shared utility header)
// ============================================================================

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

static uint64_t readBE64(const uint8_t* p)
{
    return (static_cast<uint64_t>(readBE32(p)) << 32) | readBE32(p + 4);
}

static bool isPowerOf2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

static std::string extractString(const uint8_t* data, size_t maxLen)
{
    size_t len = 0;
    while (len < maxLen && data[len] != 0)
        len++;
    while (len > 0 && data[len - 1] == ' ')
        len--;
    return std::string(reinterpret_cast<const char*>(data), len);
}

static std::string formatUuid(const uint8_t* uuid)
{
    char buf[48];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5],
             uuid[6], uuid[7],
             uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return buf;
}

std::vector<uint8_t> FilesystemInfo::safeRead(const DiskReadCallback& readFunc, uint64_t offset, uint32_t size)
{
    auto result = readFunc(offset, size);
    if (result.isError())
        return {};
    return result.value();
}

// ============================================================================
// Entry points
// ============================================================================

Result<FilesystemInfoData> FilesystemInfo::read(
    FilesystemType type,
    const DiskReadCallback& readFunc,
    uint64_t volumeSize)
{
    switch (type)
    {
    case FilesystemType::NTFS:
        return readNtfs(readFunc, volumeSize);
    case FilesystemType::FAT12:
    case FilesystemType::FAT16:
    case FilesystemType::FAT32:
        return readFat(readFunc, volumeSize);
    case FilesystemType::ExFAT:
        return readExfat(readFunc, volumeSize);
    case FilesystemType::Ext2:
    case FilesystemType::Ext3:
    case FilesystemType::Ext4:
        return readExt(readFunc, volumeSize, type);
    case FilesystemType::Btrfs:
        return readBtrfs(readFunc, volumeSize);
    case FilesystemType::XFS:
        return readXfs(readFunc, volumeSize);
    case FilesystemType::HFSPlus:
    case FilesystemType::HFS:
        return readHfsPlus(readFunc, volumeSize);
    case FilesystemType::APFS:
        return readApfs(readFunc, volumeSize);
    default:
        return readGeneric(type, readFunc, volumeSize);
    }
}

Result<FilesystemInfoData> FilesystemInfo::detectAndRead(
    const DiskReadCallback& readFunc,
    uint64_t volumeSize)
{
    auto detectResult = FilesystemDetector::detect(readFunc, volumeSize);
    if (detectResult.isError())
        return detectResult.error();

    const auto& detection = detectResult.value();
    if (!detection.isDetected())
    {
        FilesystemInfoData info;
        info.type = FilesystemType::Unknown;
        info.typeName = "Unknown";
        return info;
    }

    return read(detection.type, readFunc, volumeSize);
}

// ============================================================================
// NTFS metadata reader
//
// Boot sector layout:
//   0x00 [3]:  Jump
//   0x03 [8]:  OEM ID "NTFS    "
//   0x0B [2]:  Bytes per sector
//   0x0D [1]:  Sectors per cluster
//   0x28 [8]:  Total sectors
//   0x30 [8]:  MFT cluster number
//   0x38 [8]:  MFT mirror cluster number
//   0x40 [1]:  Clusters per MFT record (signed: if negative, size = 2^|value|)
//   0x48 [8]:  Volume serial number
//
// $Volume file (MFT record 3) contains the version info, but reading MFT
// records requires significant parsing. We extract what's available from BPB.
// ============================================================================

Result<FilesystemInfoData> FilesystemInfo::readNtfs(const DiskReadCallback& readFunc, uint64_t volumeSize)
{
    auto bpb = safeRead(readFunc, 0, 512);
    if (bpb.size() < 512)
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to read NTFS boot sector");

    FilesystemInfoData info;
    info.type = FilesystemType::NTFS;
    info.typeName = "NTFS";

    uint16_t bytesPerSector = readLE16(bpb.data() + 0x0B);
    uint8_t sectorsPerCluster = bpb[0x0D];
    uint64_t totalSectors = readLE64(bpb.data() + 0x28);

    if (bytesPerSector == 0 || sectorsPerCluster == 0)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "NTFS BPB has zero sector/cluster size");

    info.blockSize = bytesPerSector * sectorsPerCluster;
    info.totalBlocks = totalSectors / sectorsPerCluster;
    info.totalSizeBytes = totalSectors * bytesPerSector;

    info.ntfs.mftCluster = readLE64(bpb.data() + 0x30);
    info.ntfs.mftMirrorCluster = readLE64(bpb.data() + 0x38);

    // MFT record size: if byte at 0x40 is negative, size = 2^|value|
    // If positive, size = value * clustersPerMftRecord * bytesPerSector
    int8_t mftRecordVal = static_cast<int8_t>(bpb[0x40]);
    if (mftRecordVal < 0)
        info.ntfs.mftRecordSize = 1u << static_cast<uint32_t>(-mftRecordVal);
    else
        info.ntfs.mftRecordSize = static_cast<uint32_t>(mftRecordVal) * info.blockSize;

    info.ntfs.serialNumber = readLE64(bpb.data() + 0x48);

    // Format serial as UUID-like string
    if (info.ntfs.serialNumber != 0)
    {
        char buf[20];
        snprintf(buf, sizeof(buf), "%04X-%04X",
                 static_cast<unsigned>((info.ntfs.serialNumber >> 48) & 0xFFFF),
                 static_cast<unsigned>((info.ntfs.serialNumber >> 32) & 0xFFFF));
        info.uuid = buf;
    }

    // Try to read the volume label from the $Volume MFT record (#3).
    // The $Volume record is at MFT cluster + (3 * mftRecordSize / clusterSize) * clusterSize.
    // This is a complex parse — read the MFT record, find the $VOLUME_NAME attribute (0x60).
    if (info.ntfs.mftRecordSize > 0 && info.ntfs.mftCluster > 0)
    {
        uint64_t mftOffset = info.ntfs.mftCluster * info.blockSize;
        uint64_t volumeRecordOffset = mftOffset + (3ULL * info.ntfs.mftRecordSize);

        auto mftRecord = safeRead(readFunc, volumeRecordOffset, info.ntfs.mftRecordSize);
        if (mftRecord.size() >= info.ntfs.mftRecordSize)
        {
            // Validate MFT record signature "FILE"
            if (mftRecord.size() >= 4 && std::memcmp(mftRecord.data(), "FILE", 4) == 0)
            {
                // First attribute offset at byte 0x14 (uint16)
                uint16_t attrOffset = readLE16(mftRecord.data() + 0x14);

                // Walk attributes looking for $VOLUME_NAME (type 0x60)
                // and $VOLUME_INFORMATION (type 0x70)
                uint32_t pos = attrOffset;
                while (pos + 16 < info.ntfs.mftRecordSize)
                {
                    uint32_t attrType = readLE32(mftRecord.data() + pos);
                    uint32_t attrLength = readLE32(mftRecord.data() + pos + 4);

                    if (attrType == 0xFFFFFFFF || attrLength == 0)
                        break;

                    if (attrType == 0x60) // $VOLUME_NAME
                    {
                        // Resident attribute: name is at content offset
                        uint8_t nonResident = mftRecord[pos + 8];
                        if (nonResident == 0) // Resident
                        {
                            uint32_t contentSize = readLE32(mftRecord.data() + pos + 0x10);
                            uint16_t contentOffset = readLE16(mftRecord.data() + pos + 0x14);

                            if (pos + contentOffset + contentSize <= info.ntfs.mftRecordSize && contentSize > 0)
                            {
                                // UTF-16LE volume name
                                const uint16_t* nameData = reinterpret_cast<const uint16_t*>(
                                    mftRecord.data() + pos + contentOffset);
                                size_t nameChars = contentSize / 2;

                                // Convert UTF-16LE to UTF-8 (BMP only)
                                std::string name;
                                for (size_t i = 0; i < nameChars; i++)
                                {
                                    uint16_t ch = nameData[i];
                                    if (ch == 0) break;
                                    if (ch < 0x80)
                                        name.push_back(static_cast<char>(ch));
                                    else if (ch < 0x800)
                                    {
                                        name.push_back(static_cast<char>(0xC0 | (ch >> 6)));
                                        name.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                                    }
                                    else
                                    {
                                        name.push_back(static_cast<char>(0xE0 | (ch >> 12)));
                                        name.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
                                        name.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                                    }
                                }
                                info.label = name;
                            }
                        }
                    }
                    else if (attrType == 0x70) // $VOLUME_INFORMATION
                    {
                        uint8_t nonResident = mftRecord[pos + 8];
                        if (nonResident == 0)
                        {
                            uint16_t contentOffset = readLE16(mftRecord.data() + pos + 0x14);
                            uint32_t contentSize = readLE32(mftRecord.data() + pos + 0x10);

                            if (pos + contentOffset + contentSize <= info.ntfs.mftRecordSize && contentSize >= 4)
                            {
                                // $VOLUME_INFORMATION content:
                                //   0x00 [8]: reserved
                                //   0x08 [1]: major version
                                //   0x09 [1]: minor version
                                //   0x0A [2]: flags
                                const uint8_t* viData = mftRecord.data() + pos + contentOffset;
                                if (contentSize >= 12)
                                {
                                    info.ntfs.majorVersion = viData[8];
                                    info.ntfs.minorVersion = viData[9];
                                }
                                else if (contentSize >= 4)
                                {
                                    // Compact format
                                    info.ntfs.majorVersion = viData[0];
                                    info.ntfs.minorVersion = viData[1];
                                }
                            }
                        }
                    }

                    pos += attrLength;
                }
            }
        }
    }

    return info;
}

// ============================================================================
// FAT metadata reader
//
// Common BPB (BIOS Parameter Block) layout:
//   0x00 [3]:   Jump instruction
//   0x03 [8]:   OEM name
//   0x0B [2]:   Bytes per sector
//   0x0D [1]:   Sectors per cluster
//   0x0E [2]:   Reserved sectors
//   0x10 [1]:   Number of FATs
//   0x11 [2]:   Root entry count (FAT12/16 only)
//   0x13 [2]:   Total sectors (16-bit, 0 if 32-bit used)
//   0x15 [1]:   Media type
//   0x16 [2]:   FAT size (16-bit, 0 for FAT32)
//   0x20 [4]:   Total sectors (32-bit)
//
// FAT32 extended BPB:
//   0x24 [4]:   FAT size (32-bit)
//   0x2C [4]:   Root cluster
//   0x43 [4]:   Volume serial
//   0x47 [11]:  Volume label
//
// FAT12/16 extended BPB:
//   0x27 [4]:   Volume serial
//   0x2B [11]:  Volume label
// ============================================================================

Result<FilesystemInfoData> FilesystemInfo::readFat(const DiskReadCallback& readFunc, uint64_t volumeSize)
{
    auto bpb = safeRead(readFunc, 0, 512);
    if (bpb.size() < 512)
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to read FAT boot sector");

    FilesystemInfoData info;

    uint16_t bytesPerSector = readLE16(bpb.data() + 0x0B);
    uint8_t sectorsPerCluster = bpb[0x0D];
    uint16_t reservedSectors = readLE16(bpb.data() + 0x0E);
    uint8_t numFats = bpb[0x10];
    uint16_t rootEntryCount = readLE16(bpb.data() + 0x11);
    uint16_t totalSectors16 = readLE16(bpb.data() + 0x13);
    uint16_t fatSize16 = readLE16(bpb.data() + 0x16);
    uint32_t totalSectors32 = readLE32(bpb.data() + 0x20);

    if (bytesPerSector == 0 || sectorsPerCluster == 0)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "FAT BPB has zero values");

    uint32_t fatSize = fatSize16;
    bool isFat32 = (fatSize == 0);
    if (isFat32)
        fatSize = readLE32(bpb.data() + 0x24);

    uint32_t totalSectors = (totalSectors16 != 0) ? totalSectors16 : totalSectors32;
    uint32_t rootDirSectors = ((rootEntryCount * 32) + (bytesPerSector - 1)) / bytesPerSector;
    uint32_t dataStartSector = reservedSectors + (numFats * fatSize) + rootDirSectors;
    uint32_t dataSectors = (totalSectors > dataStartSector) ? totalSectors - dataStartSector : 0;
    uint32_t totalClusters = (sectorsPerCluster > 0) ? dataSectors / sectorsPerCluster : 0;

    // Determine FAT type
    if (totalClusters < 4085)
    {
        info.type = FilesystemType::FAT12;
        info.typeName = "FAT12";
    }
    else if (totalClusters < 65525)
    {
        info.type = FilesystemType::FAT16;
        info.typeName = "FAT16";
    }
    else
    {
        info.type = FilesystemType::FAT32;
        info.typeName = "FAT32";
    }

    info.blockSize = static_cast<uint32_t>(bytesPerSector) * sectorsPerCluster;
    info.totalBlocks = totalClusters;
    info.totalSizeBytes = static_cast<uint64_t>(totalSectors) * bytesPerSector;

    info.fat.fatCount = numFats;
    info.fat.fatSize = fatSize;
    info.fat.reservedSectors = reservedSectors;
    info.fat.rootEntryCount = rootEntryCount;
    info.fat.totalClusters = totalClusters;

    // OEM name
    info.fat.oemName = extractString(bpb.data() + 3, 8);

    // Volume label and serial
    if (isFat32)
    {
        info.fat.volumeSerial = readLE32(bpb.data() + 0x43);
        info.label = extractString(bpb.data() + 0x47, 11);
    }
    else
    {
        info.fat.volumeSerial = readLE32(bpb.data() + 0x27);
        info.label = extractString(bpb.data() + 0x2B, 11);
    }

    if (info.label == "NO NAME")
        info.label.clear();

    if (info.fat.volumeSerial != 0)
    {
        char buf[12];
        snprintf(buf, sizeof(buf), "%04X-%04X",
                 (info.fat.volumeSerial >> 16) & 0xFFFF,
                 info.fat.volumeSerial & 0xFFFF);
        info.uuid = buf;
    }

    // Count free clusters by scanning the FAT
    // For FAT32, read the FSInfo sector at sector 1 (offset 0x1E8 has free count)
    if (isFat32)
    {
        uint16_t fsInfoSector = readLE16(bpb.data() + 0x30);
        if (fsInfoSector > 0 && fsInfoSector < reservedSectors)
        {
            auto fsInfo = safeRead(readFunc,
                static_cast<uint64_t>(fsInfoSector) * bytesPerSector, 512);
            if (fsInfo.size() >= 512)
            {
                // FSInfo signatures: 0x41615252 at offset 0, 0x61417272 at offset 484
                uint32_t sig1 = readLE32(fsInfo.data());
                uint32_t sig2 = readLE32(fsInfo.data() + 484);
                if (sig1 == 0x41615252u && sig2 == 0x61417272u)
                {
                    uint32_t freeClusters = readLE32(fsInfo.data() + 488);
                    if (freeClusters != 0xFFFFFFFFu) // 0xFFFFFFFF = unknown
                    {
                        info.freeBlocks = freeClusters;
                        info.freeSizeBytes = static_cast<uint64_t>(freeClusters) * info.blockSize;
                        info.usedSizeBytes = info.totalSizeBytes - info.freeSizeBytes;
                    }
                }
            }
        }
    }

    return info;
}

// ============================================================================
// exFAT metadata reader
//
// Boot sector layout:
//   0x00 [3]:   Jump
//   0x03 [8]:   "EXFAT   "
//   0x40 [8]:   Partition offset (sectors)
//   0x48 [8]:   Volume length (sectors)
//   0x50 [4]:   FAT offset (sectors)
//   0x54 [4]:   FAT length (sectors)
//   0x58 [4]:   Cluster heap offset (sectors)
//   0x5C [4]:   Cluster count
//   0x60 [4]:   First cluster of root directory
//   0x64 [4]:   Volume serial number
//   0x68 [2]:   FS revision
//   0x6C [1]:   BytesPerSectorShift
//   0x6D [1]:   SectorsPerClusterShift
//   0x6E [1]:   Number of FATs
// ============================================================================

Result<FilesystemInfoData> FilesystemInfo::readExfat(const DiskReadCallback& readFunc, uint64_t volumeSize)
{
    auto boot = safeRead(readFunc, 0, 512);
    if (boot.size() < 512)
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to read exFAT boot sector");

    FilesystemInfoData info;
    info.type = FilesystemType::ExFAT;
    info.typeName = "exFAT";

    uint64_t volumeLength = readLE64(boot.data() + 0x48);
    uint32_t clusterCount = readLE32(boot.data() + 0x5C);
    uint32_t volumeSerial = readLE32(boot.data() + 0x64);
    uint16_t fsRevision = readLE16(boot.data() + 0x68);

    uint8_t bytesPerSectorShift = boot[0x6C];
    uint8_t sectorsPerClusterShift = boot[0x6D];

    uint32_t bytesPerSector = (bytesPerSectorShift <= 12) ? (1u << bytesPerSectorShift) : 512;
    uint32_t sectorsPerCluster = (sectorsPerClusterShift <= 25) ? (1u << sectorsPerClusterShift) : 1;

    info.blockSize = bytesPerSector * sectorsPerCluster;
    info.totalBlocks = clusterCount;
    info.totalSizeBytes = volumeLength * bytesPerSector;

    info.exfat.fsRevision = fsRevision;
    info.exfat.clusterCount = clusterCount;
    info.exfat.volumeSerial = volumeSerial;

    if (volumeSerial != 0)
    {
        char buf[12];
        snprintf(buf, sizeof(buf), "%04X-%04X",
                 (volumeSerial >> 16) & 0xFFFF,
                 volumeSerial & 0xFFFF);
        info.uuid = buf;
    }

    // exFAT volume label is stored in the root directory as a Volume Label entry (type 0x83).
    // Read the root directory cluster to find it.
    uint32_t rootCluster = readLE32(boot.data() + 0x60);
    uint32_t clusterHeapOffset = readLE32(boot.data() + 0x58);

    if (rootCluster >= 2)
    {
        uint64_t rootOffset = static_cast<uint64_t>(clusterHeapOffset + (rootCluster - 2) * sectorsPerCluster) * bytesPerSector;
        auto rootData = safeRead(readFunc, rootOffset, info.blockSize);

        if (rootData.size() >= 32)
        {
            // Scan for Volume Label entry (entry type 0x83)
            for (size_t pos = 0; pos + 32 <= rootData.size(); pos += 32)
            {
                uint8_t entryType = rootData[pos];
                if (entryType == 0x83) // Volume Label
                {
                    uint8_t charCount = rootData[pos + 1];
                    if (charCount > 11) charCount = 11;

                    // Label is UTF-16LE starting at offset 2
                    const uint16_t* labelData = reinterpret_cast<const uint16_t*>(&rootData[pos + 2]);
                    std::string label;
                    for (int i = 0; i < charCount; i++)
                    {
                        uint16_t ch = labelData[i];
                        if (ch < 0x80)
                            label.push_back(static_cast<char>(ch));
                        else if (ch < 0x800)
                        {
                            label.push_back(static_cast<char>(0xC0 | (ch >> 6)));
                            label.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                        }
                        else
                        {
                            label.push_back(static_cast<char>(0xE0 | (ch >> 12)));
                            label.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
                            label.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                        }
                    }
                    info.label = label;
                    break;
                }
                else if (entryType == 0x00)
                {
                    break; // End of directory
                }
            }
        }
    }

    return info;
}

// ============================================================================
// ext2/3/4 metadata reader
//
// Superblock at byte offset 1024, size 1024 bytes.
// All fields are little-endian.
//
// Key superblock offsets (relative to superblock start):
//   0x00 [4]:  s_inodes_count
//   0x04 [4]:  s_blocks_count_lo
//   0x08 [4]:  s_r_blocks_count_lo
//   0x0C [4]:  s_free_blocks_count_lo
//   0x10 [4]:  s_free_inodes_count
//   0x14 [4]:  s_first_data_block
//   0x18 [4]:  s_log_block_size (block size = 1024 << value)
//   0x38 [2]:  s_magic (0xEF53)
//   0x3A [2]:  s_state
//   0x3C [2]:  s_errors
//   0x48 [4]:  s_creator_os
//   0x5C [4]:  s_feature_compat
//   0x60 [4]:  s_feature_incompat
//   0x64 [4]:  s_feature_ro_compat
//   0x68 [16]: s_uuid
//   0x78 [16]: s_volume_name
//   0x150 [4]: s_blocks_count_hi (ext4 64-bit)
//   0x158 [4]: s_free_blocks_count_hi (ext4 64-bit)
// ============================================================================

Result<FilesystemInfoData> FilesystemInfo::readExt(const DiskReadCallback& readFunc, uint64_t volumeSize, FilesystemType type)
{
    auto sb = safeRead(readFunc, 1024, 1024);
    if (sb.size() < 256)
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to read ext superblock");

    // Verify magic
    uint16_t magic = readLE16(sb.data() + 0x38);
    if (magic != EXT_SUPER_MAGIC)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "Invalid ext superblock magic");

    FilesystemInfoData info;
    info.type = type;
    info.typeName = FilesystemDetector::filesystemName(type);

    // Block size
    uint32_t logBlockSize = readLE32(sb.data() + 0x18);
    info.blockSize = (logBlockSize < 10) ? (1024u << logBlockSize) : 4096;

    // Block counts
    uint32_t blocksLo = readLE32(sb.data() + 0x04);
    uint32_t freeBlocksLo = readLE32(sb.data() + 0x0C);

    uint32_t compatFeatures = readLE32(sb.data() + 0x5C);
    uint32_t incompatFeatures = readLE32(sb.data() + 0x60);
    uint32_t roCompatFeatures = readLE32(sb.data() + 0x64);

    // 64-bit block counts for ext4
    uint64_t totalBlocks = blocksLo;
    uint64_t freeBlocks = freeBlocksLo;

    if ((incompatFeatures & ExtFeatures::Incompat_64bit) && sb.size() >= 0x15C)
    {
        uint32_t blocksHi = readLE32(sb.data() + 0x150);
        uint32_t freeBlocksHi = readLE32(sb.data() + 0x158);
        totalBlocks |= (static_cast<uint64_t>(blocksHi) << 32);
        freeBlocks |= (static_cast<uint64_t>(freeBlocksHi) << 32);
    }

    info.totalBlocks = totalBlocks;
    info.freeBlocks = freeBlocks;
    info.totalSizeBytes = totalBlocks * info.blockSize;
    info.freeSizeBytes = freeBlocks * info.blockSize;
    info.usedSizeBytes = info.totalSizeBytes - info.freeSizeBytes;

    // Inodes
    info.ext.inodeCount = readLE32(sb.data() + 0x00);
    info.ext.freeInodes = readLE32(sb.data() + 0x10);

    // Block groups
    uint32_t blocksPerGroup = readLE32(sb.data() + 0x20);
    if (blocksPerGroup > 0)
        info.ext.blockGroupCount = static_cast<uint32_t>((totalBlocks + blocksPerGroup - 1) / blocksPerGroup);

    // State and error handling
    info.ext.state = readLE16(sb.data() + 0x3A);
    info.ext.errors = readLE16(sb.data() + 0x3C);
    info.ext.creatorOs = readLE32(sb.data() + 0x48);

    // Features
    info.ext.compatFeatures = compatFeatures;
    info.ext.incompatFeatures = incompatFeatures;
    info.ext.roCompatFeatures = roCompatFeatures;

    // Build human-readable feature list
    auto& fs = info.ext.featureStrings;
    if (compatFeatures & ExtFeatures::Compat_HasJournal)   fs.push_back("has_journal");
    if (compatFeatures & ExtFeatures::Compat_DirIndex)     fs.push_back("dir_index");
    if (incompatFeatures & ExtFeatures::Incompat_Filetype)  fs.push_back("filetype");
    if (incompatFeatures & ExtFeatures::Incompat_Extents)   fs.push_back("extents");
    if (incompatFeatures & ExtFeatures::Incompat_64bit)     fs.push_back("64bit");
    if (incompatFeatures & ExtFeatures::Incompat_FlexBg)    fs.push_back("flex_bg");
    if (roCompatFeatures & ExtFeatures::RoCompat_Sparse)    fs.push_back("sparse_super");
    if (roCompatFeatures & ExtFeatures::RoCompat_LargeFile) fs.push_back("large_file");
    if (roCompatFeatures & ExtFeatures::RoCompat_HugeFile)  fs.push_back("huge_file");
    if (roCompatFeatures & ExtFeatures::RoCompat_Metadata)  fs.push_back("metadata_csum");

    // Label
    info.label = extractString(sb.data() + 0x78, 16);

    // UUID
    info.uuid = formatUuid(sb.data() + 0x68);

    return info;
}

// ============================================================================
// Btrfs metadata reader
//
// Superblock at offset 0x10000 (64 KiB).
// Key offsets relative to superblock start:
//   0x00 [32]: csum
//   0x20 [16]: fsid (UUID)
//   0x30 [8]:  bytenr (physical offset of this block)
//   0x40 [8]:  magic "_BHRfS_M"
//   0x48 [8]:  generation
//   0x50 [8]:  root
//   0x58 [8]:  chunk_root
//   0x60 [8]:  log_root
//   0x80 [4]:  sectorsize
//   0x84 [4]:  nodesize
//   0x8C [8]:  total_bytes
//   0x94 [8]:  bytes_used
//   0x12B [256]: label
// ============================================================================

Result<FilesystemInfoData> FilesystemInfo::readBtrfs(const DiskReadCallback& readFunc, uint64_t volumeSize)
{
    auto sb = safeRead(readFunc, 0x10000, 0x200);
    if (sb.size() < 0x1A0)
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to read Btrfs superblock");

    FilesystemInfoData info;
    info.type = FilesystemType::Btrfs;
    info.typeName = "Btrfs";

    info.blockSize = readLE32(sb.data() + 0x80);     // sectorsize
    info.totalSizeBytes = readLE64(sb.data() + 0x8C); // total_bytes
    info.usedSizeBytes = readLE64(sb.data() + 0x94);  // bytes_used
    info.freeSizeBytes = (info.totalSizeBytes > info.usedSizeBytes) ?
                          info.totalSizeBytes - info.usedSizeBytes : 0;

    if (info.blockSize > 0)
    {
        info.totalBlocks = info.totalSizeBytes / info.blockSize;
        info.freeBlocks = info.freeSizeBytes / info.blockSize;
    }

    // UUID (fsid) at offset 0x20
    info.uuid = formatUuid(sb.data() + 0x20);

    // Label at offset 0x12B (256 bytes)
    if (sb.size() > 0x12B + 256)
        info.label = extractString(sb.data() + 0x12B, 256);

    return info;
}

// ============================================================================
// XFS metadata reader
//
// Superblock at offset 0, all fields big-endian.
// Key offsets:
//   0x00 [4]:  sb_magicnum "XFSB"
//   0x04 [4]:  sb_blocksize
//   0x08 [8]:  sb_dblocks (total data blocks)
//   0x10 [8]:  sb_rblocks (realtime blocks)
//   0x18 [8]:  sb_rextents
//   0x20 [16]: sb_uuid
//   0x60 [8]:  sb_fdblocks (free data blocks)
//   0x68 [8]:  sb_icount
//   0x70 [8]:  sb_ifree
//   0x6C [12]: sb_fname (label)
// ============================================================================

Result<FilesystemInfoData> FilesystemInfo::readXfs(const DiskReadCallback& readFunc, uint64_t volumeSize)
{
    auto sb = safeRead(readFunc, 0, 512);
    if (sb.size() < 256)
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to read XFS superblock");

    FilesystemInfoData info;
    info.type = FilesystemType::XFS;
    info.typeName = "XFS";

    info.blockSize = readBE32(sb.data() + 0x04);
    uint64_t totalBlocks = readBE64(sb.data() + 0x08);
    uint64_t freeBlocks = readBE64(sb.data() + 0x60);

    info.totalBlocks = totalBlocks;
    info.freeBlocks = freeBlocks;
    info.totalSizeBytes = totalBlocks * info.blockSize;
    info.freeSizeBytes = freeBlocks * info.blockSize;
    info.usedSizeBytes = info.totalSizeBytes - info.freeSizeBytes;

    // Label at offset 0x6C (12 bytes)
    info.label = extractString(sb.data() + 0x6C, 12);

    // UUID at offset 0x20
    info.uuid = formatUuid(sb.data() + 0x20);

    return info;
}

// ============================================================================
// HFS+ metadata reader
//
// Volume header at offset 1024 (byte offset from partition start), big-endian.
// Key offsets:
//   0x00 [2]:  signature (0x482B "H+" or 0x4858 "HX")
//   0x02 [2]:  version
//   0x04 [4]:  attributes
//   0x12 [2]:  modify date
//   0x1C [4]:  fileCount
//   0x20 [4]:  folderCount
//   0x28 [4]:  blockSize
//   0x2C [4]:  totalBlocks
//   0x30 [4]:  freeBlocks
// ============================================================================

Result<FilesystemInfoData> FilesystemInfo::readHfsPlus(const DiskReadCallback& readFunc, uint64_t volumeSize)
{
    auto vh = safeRead(readFunc, 1024, 512);
    if (vh.size() < 162)
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to read HFS+ volume header");

    uint16_t sig = readBE16(vh.data());

    FilesystemInfoData info;
    if (sig == 0x4244) // Classic HFS
    {
        info.type = FilesystemType::HFS;
        info.typeName = "HFS (Classic)";
        // Basic HFS Master Directory Block parsing
        info.blockSize = readBE32(vh.data() + 0x14); // drAlBlkSiz
        uint16_t numABlocks = readBE16(vh.data() + 0x12); // drNmAlBlks
        uint16_t freeABlocks = readBE16(vh.data() + 0x22); // drFreeBks
        info.totalBlocks = numABlocks;
        info.freeBlocks = freeABlocks;
        info.totalSizeBytes = static_cast<uint64_t>(numABlocks) * info.blockSize;
        info.freeSizeBytes = static_cast<uint64_t>(freeABlocks) * info.blockSize;
        info.usedSizeBytes = info.totalSizeBytes - info.freeSizeBytes;

        // Volume name at offset 0x25 (Pascal string: length byte + chars)
        uint8_t nameLen = vh[0x24];
        if (nameLen > 0 && nameLen <= 27)
            info.label = extractString(vh.data() + 0x25, nameLen);

        return info;
    }

    info.type = FilesystemType::HFSPlus;
    info.typeName = (sig == HFSX_MAGIC) ? "HFSX" : "HFS+";

    info.hfsplus.version = readBE16(vh.data() + 0x02);
    info.hfsplus.fileCount = readBE32(vh.data() + 0x1C);
    info.hfsplus.folderCount = readBE32(vh.data() + 0x20);

    info.blockSize = readBE32(vh.data() + 0x28);
    uint32_t totalBlocks = readBE32(vh.data() + 0x2C);
    uint32_t freeBlocks = readBE32(vh.data() + 0x30);

    info.totalBlocks = totalBlocks;
    info.freeBlocks = freeBlocks;
    info.totalSizeBytes = static_cast<uint64_t>(totalBlocks) * info.blockSize;
    info.freeSizeBytes = static_cast<uint64_t>(freeBlocks) * info.blockSize;
    info.usedSizeBytes = info.totalSizeBytes - info.freeSizeBytes;

    // HFS+ stores the volume name in the catalog file, which requires B-tree traversal.
    // This is complex — we leave label empty for HFS+ unless the caller provides it.

    return info;
}

// ============================================================================
// APFS metadata reader
//
// Container superblock (NXSB) at offset 0, little-endian.
// Fields after the 32-byte object header (obj_phys_t):
//   +0x00 [4]:  nx_magic (0x4253584E "NXSB")
//   +0x04 [4]:  nx_block_size
//   +0x08 [8]:  nx_block_count
//   +0x18 [16]: nx_uuid
// ============================================================================

Result<FilesystemInfoData> FilesystemInfo::readApfs(const DiskReadCallback& readFunc, uint64_t volumeSize)
{
    auto nxsb = safeRead(readFunc, 0, 4096);
    if (nxsb.size() < 128)
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Failed to read APFS container superblock");

    FilesystemInfoData info;
    info.type = FilesystemType::APFS;
    info.typeName = "APFS";

    // Object header is 32 bytes, then container superblock fields
    info.blockSize = readLE32(nxsb.data() + 36);   // nx_block_size
    uint64_t blockCount = readLE64(nxsb.data() + 40); // nx_block_count

    info.totalBlocks = blockCount;
    info.totalSizeBytes = blockCount * info.blockSize;

    // UUID at offset 32+24 = 56 (nx_uuid)
    if (nxsb.size() >= 72)
        info.uuid = formatUuid(nxsb.data() + 56);

    // APFS stores volume names in volume superblocks, which are referenced through
    // the object map. Full parsing would require B-tree traversal of the omap.
    // We leave label empty for the container level.

    return info;
}

// ============================================================================
// Generic metadata reader — for filesystems where we have limited parsing
// ============================================================================

Result<FilesystemInfoData> FilesystemInfo::readGeneric(FilesystemType type, const DiskReadCallback& readFunc, uint64_t volumeSize)
{
    // Use detection results as the baseline for less common filesystems
    auto detectResult = FilesystemDetector::detect(readFunc, volumeSize);
    if (detectResult.isError())
        return detectResult.error();

    const auto& det = detectResult.value();

    FilesystemInfoData info;
    info.type = type;
    info.typeName = FilesystemDetector::filesystemName(type);
    info.label = det.label;
    info.uuid = det.uuid;
    info.blockSize = det.blockSize;
    info.totalSizeBytes = volumeSize;

    return info;
}

} // namespace spw
