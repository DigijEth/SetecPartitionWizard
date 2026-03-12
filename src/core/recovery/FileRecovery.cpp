// FileRecovery.cpp -- Recover deleted files from NTFS/FAT/ext and via file carving.
//
// DISCLAIMER: This code is for authorized disk utility / forensics software only.

#include "FileRecovery.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace spw
{

// ---------------------------------------------------------------------------
// NTFS on-disk structures (packed, little-endian)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)

// MFT record header (FILE record)
struct NtfsMftHeader
{
    char     magic[4];           // "FILE"
    uint16_t updateSeqOffset;
    uint16_t updateSeqCount;
    uint64_t logSeqNumber;
    uint16_t sequenceNumber;
    uint16_t hardLinkCount;
    uint16_t firstAttributeOffset;
    uint16_t flags;              // 0x01 = in use, 0x02 = directory
    uint32_t realSize;
    uint32_t allocatedSize;
    uint64_t baseRecord;
    uint16_t nextAttributeId;
    uint16_t padding;
    uint32_t mftRecordNumber;
};

// NTFS attribute header (common prefix for resident and non-resident)
struct NtfsAttributeHeader
{
    uint32_t type;               // Attribute type (0x30 = $FILE_NAME, 0x80 = $DATA)
    uint32_t length;             // Total length of this attribute
    uint8_t  nonResident;        // 0 = resident, 1 = non-resident
    uint8_t  nameLength;
    uint16_t nameOffset;
    uint16_t flags;
    uint16_t attributeId;
};

// Resident attribute portion (follows NtfsAttributeHeader when nonResident == 0)
struct NtfsResidentAttr
{
    uint32_t valueLength;
    uint16_t valueOffset;
    uint16_t indexedFlag;
};

// Non-resident attribute portion (follows NtfsAttributeHeader when nonResident == 1)
struct NtfsNonResidentAttr
{
    uint64_t startingVcn;
    uint64_t lastVcn;
    uint16_t dataRunsOffset;
    uint16_t compressionUnit;
    uint32_t padding;
    uint64_t allocatedSize;
    uint64_t realSize;
    uint64_t initializedSize;
};

// $FILE_NAME attribute body (partial -- we only need the name)
struct NtfsFileNameAttr
{
    uint64_t parentDirectory;
    uint64_t creationTime;
    uint64_t modifiedTime;
    uint64_t mftModifiedTime;
    uint64_t accessTime;
    uint64_t allocatedSize;
    uint64_t realSize;
    uint32_t flags;
    uint32_t reparseValue;
    uint8_t  fileNameLength;     // In UTF-16 characters
    uint8_t  fileNameNamespace;  // 0=POSIX, 1=Win32, 2=DOS, 3=Win32+DOS
    // Followed by wchar_t fileName[fileNameLength]
};

// FAT directory entry (32 bytes)
struct FatDirEntry
{
    uint8_t  name[11];           // 8.3 filename, first byte 0xE5 = deleted
    uint8_t  attributes;
    uint8_t  ntReserved;
    uint8_t  createTimeTenths;
    uint16_t createTime;
    uint16_t createDate;
    uint16_t accessDate;
    uint16_t firstClusterHigh;   // High 16 bits of first cluster (FAT32 only)
    uint16_t writeTime;
    uint16_t writeDate;
    uint16_t firstClusterLow;
    uint32_t fileSize;
};

#pragma pack(pop)

// NTFS attribute type constants
constexpr uint32_t NTFS_ATTR_STANDARD_INFO = 0x10;
constexpr uint32_t NTFS_ATTR_FILE_NAME     = 0x30;
constexpr uint32_t NTFS_ATTR_DATA          = 0x80;
constexpr uint32_t NTFS_ATTR_END           = 0xFFFFFFFF;

// MFT record flag bits
constexpr uint16_t NTFS_MFT_FLAG_IN_USE    = 0x0001;
constexpr uint16_t NTFS_MFT_FLAG_DIRECTORY  = 0x0002;

// FAT constants
constexpr uint8_t FAT_DELETED_MARKER = 0xE5;
constexpr uint8_t FAT_ATTR_LONG_NAME = 0x0F;
constexpr uint8_t FAT_ATTR_VOLUME_ID = 0x08;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

FileRecovery::FileRecovery(RawDiskHandle& disk,
                           SectorOffset partitionStartLba,
                           SectorCount partitionSectorCount,
                           FilesystemType fsType,
                           uint32_t sectorSize)
    : m_disk(disk)
    , m_partStart(partitionStartLba)
    , m_partSectors(partitionSectorCount)
    , m_fsType(fsType)
    , m_sectorSize(sectorSize)
{
}

// ---------------------------------------------------------------------------
// readPartitionBytes -- read bytes relative to partition start
// ---------------------------------------------------------------------------

Result<std::vector<uint8_t>> FileRecovery::readPartitionBytes(uint64_t offset, uint32_t size) const
{
    uint64_t absOffset = (m_partStart * m_sectorSize) + offset;
    SectorOffset startSector = absOffset / m_sectorSize;
    uint32_t inSectorOffset = static_cast<uint32_t>(absOffset % m_sectorSize);
    uint32_t alignedSize = ((inSectorOffset + size + m_sectorSize - 1) / m_sectorSize) * m_sectorSize;
    SectorCount sectorsToRead = alignedSize / m_sectorSize;

    auto readResult = m_disk.readSectors(startSector, sectorsToRead, m_sectorSize);
    if (readResult.isError())
        return readResult.error();

    auto& data = readResult.value();
    if (inSectorOffset + size > data.size())
        return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Partition read underflow");

    return std::vector<uint8_t>(data.begin() + inSectorOffset,
                                 data.begin() + inSectorOffset + size);
}

// ---------------------------------------------------------------------------
// scan -- main entry point
// ---------------------------------------------------------------------------

Result<std::vector<RecoverableFile>> FileRecovery::scan(
    FileRecoveryMode mode,
    FileRecoveryProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    std::vector<RecoverableFile> allResults;

    // Filesystem-aware scan
    if (mode == FileRecoveryMode::FilesystemAware || mode == FileRecoveryMode::Both)
    {
        Result<std::vector<RecoverableFile>> fsResult =
            ErrorInfo::fromCode(ErrorCode::FilesystemNotSupported);

        switch (m_fsType)
        {
        case FilesystemType::NTFS:
            fsResult = scanNtfs(progressCb, cancelFlag);
            break;
        case FilesystemType::FAT12:
        case FilesystemType::FAT16:
        case FilesystemType::FAT32:
            fsResult = scanFat(progressCb, cancelFlag);
            break;
        case FilesystemType::Ext2:
        case FilesystemType::Ext3:
        case FilesystemType::Ext4:
            fsResult = scanExt(progressCb, cancelFlag);
            break;
        default:
            // Not supported for FS-aware scanning; carving will handle it if Both
            break;
        }

        if (fsResult.isOk())
        {
            auto& files = fsResult.value();
            allResults.insert(allResults.end(),
                              std::make_move_iterator(files.begin()),
                              std::make_move_iterator(files.end()));
        }
    }

    // File carving pass
    if (mode == FileRecoveryMode::Carving || mode == FileRecoveryMode::Both)
    {
        auto carveResult = scanCarving(progressCb, cancelFlag);
        if (carveResult.isOk())
        {
            auto& files = carveResult.value();
            allResults.insert(allResults.end(),
                              std::make_move_iterator(files.begin()),
                              std::make_move_iterator(files.end()));
        }
    }

    if (allResults.empty())
        return ErrorInfo::fromCode(ErrorCode::NoFilesRecovered, "No recoverable files found");

    return allResults;
}

// ---------------------------------------------------------------------------
// scanNtfs -- scan MFT for deleted entries
// ---------------------------------------------------------------------------

Result<std::vector<RecoverableFile>> FileRecovery::scanNtfs(
    FileRecoveryProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    // Read the NTFS boot sector to find MFT location
    auto bootResult = readPartitionBytes(0, 512);
    if (bootResult.isError())
        return bootResult.error();

    const auto& boot = bootResult.value();
    if (boot.size() < 512)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "NTFS boot sector too small");

    // Verify NTFS signature at offset 3
    if (std::memcmp(&boot[3], "NTFS    ", 8) != 0)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "Not an NTFS volume");

    // Bytes per sector: offset 0x0B (2 bytes)
    uint16_t bytesPerSector = 0;
    std::memcpy(&bytesPerSector, &boot[0x0B], 2);
    if (bytesPerSector == 0)
        bytesPerSector = static_cast<uint16_t>(m_sectorSize);

    // Sectors per cluster: offset 0x0D (1 byte)
    uint8_t sectorsPerCluster = boot[0x0D];
    if (sectorsPerCluster == 0)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "NTFS sectors/cluster is 0");

    uint32_t clusterSize = static_cast<uint32_t>(bytesPerSector) * sectorsPerCluster;

    // MFT cluster number: offset 0x30 (8 bytes)
    uint64_t mftCluster = 0;
    std::memcpy(&mftCluster, &boot[0x30], 8);

    // MFT record size: offset 0x40 (signed byte, clusters or 2^(-val) if negative)
    int8_t mftRecordSizeRaw = static_cast<int8_t>(boot[0x40]);
    uint32_t mftRecordSize = 0;
    if (mftRecordSizeRaw > 0)
        mftRecordSize = static_cast<uint32_t>(mftRecordSizeRaw) * clusterSize;
    else
        mftRecordSize = 1u << static_cast<uint32_t>(-mftRecordSizeRaw);

    if (mftRecordSize == 0 || mftRecordSize > 65536)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "Invalid MFT record size");

    uint64_t mftByteOffset = mftCluster * clusterSize;

    // Scan the MFT. We'll read records one at a time and look for deleted entries.
    // We scan up to a reasonable number of records (limit to prevent infinite reads).
    const uint64_t partSizeBytes = m_partSectors * m_sectorSize;
    const uint64_t maxMftRecords = (partSizeBytes - mftByteOffset) / mftRecordSize;
    // Cap at 1 million records to keep the scan bounded
    const uint64_t recordsToScan = std::min(maxMftRecords, static_cast<uint64_t>(1000000));

    std::vector<RecoverableFile> results;

    for (uint64_t recordIdx = 0; recordIdx < recordsToScan; ++recordIdx)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled);

        uint64_t recordOffset = mftByteOffset + (recordIdx * mftRecordSize);
        auto recordResult = readPartitionBytes(recordOffset, mftRecordSize);
        if (recordResult.isError())
            break; // Past end of readable area

        const auto& recordData = recordResult.value();
        if (recordData.size() < sizeof(NtfsMftHeader))
            continue;

        // Verify FILE signature
        if (std::memcmp(recordData.data(), "FILE", 4) != 0)
            continue;

        NtfsMftHeader header;
        std::memcpy(&header, recordData.data(), sizeof(header));

        // We want DELETED entries: magic == "FILE" but flags & IN_USE == 0
        if (header.flags & NTFS_MFT_FLAG_IN_USE)
            continue; // Still in use, not deleted

        // Skip directories
        if (header.flags & NTFS_MFT_FLAG_DIRECTORY)
            continue;

        // Walk attributes looking for $FILE_NAME (0x30) and $DATA (0x80)
        std::string fileName;
        uint64_t    fileSize = 0;
        std::vector<RecoverableFile::DataRun> dataRuns;

        uint32_t attrOffset = header.firstAttributeOffset;
        while (attrOffset + sizeof(NtfsAttributeHeader) <= recordData.size())
        {
            NtfsAttributeHeader attrHeader;
            std::memcpy(&attrHeader, &recordData[attrOffset], sizeof(attrHeader));

            if (attrHeader.type == NTFS_ATTR_END || attrHeader.length == 0)
                break;

            if (attrOffset + attrHeader.length > recordData.size())
                break;

            if (attrHeader.type == NTFS_ATTR_FILE_NAME && attrHeader.nonResident == 0)
            {
                // Resident $FILE_NAME attribute
                NtfsResidentAttr resident;
                if (attrOffset + sizeof(NtfsAttributeHeader) + sizeof(resident) <= recordData.size())
                {
                    std::memcpy(&resident, &recordData[attrOffset + sizeof(NtfsAttributeHeader)],
                                sizeof(resident));

                    uint32_t nameAttrOffset = attrOffset + resident.valueOffset;
                    if (nameAttrOffset + sizeof(NtfsFileNameAttr) <= recordData.size())
                    {
                        NtfsFileNameAttr fnAttr;
                        std::memcpy(&fnAttr, &recordData[nameAttrOffset], sizeof(fnAttr));

                        // Skip DOS-only names (namespace 2), prefer Win32 (1) or Win32+DOS (3)
                        if (fnAttr.fileNameNamespace != 2)
                        {
                            uint32_t nameStart = nameAttrOffset + sizeof(NtfsFileNameAttr);
                            uint32_t nameBytes = static_cast<uint32_t>(fnAttr.fileNameLength) * 2;
                            if (nameStart + nameBytes <= recordData.size())
                            {
                                // Convert UTF-16LE to UTF-8 (simple ASCII conversion)
                                fileName.clear();
                                for (uint32_t i = 0; i < fnAttr.fileNameLength; ++i)
                                {
                                    uint16_t ch = 0;
                                    std::memcpy(&ch, &recordData[nameStart + i * 2], 2);
                                    if (ch < 128)
                                        fileName.push_back(static_cast<char>(ch));
                                    else
                                        fileName.push_back('?'); // Non-ASCII placeholder
                                }
                            }
                        }
                    }
                }
            }
            else if (attrHeader.type == NTFS_ATTR_DATA)
            {
                if (attrHeader.nonResident == 1)
                {
                    // Non-resident $DATA: parse the data run list
                    NtfsNonResidentAttr nonRes;
                    if (attrOffset + sizeof(NtfsAttributeHeader) + sizeof(nonRes) <= recordData.size())
                    {
                        std::memcpy(&nonRes, &recordData[attrOffset + sizeof(NtfsAttributeHeader)],
                                    sizeof(nonRes));
                        fileSize = nonRes.realSize;

                        // Parse data runs. Each run is encoded as:
                        //   header byte: low nibble = length-field size,
                        //                high nibble = offset-field size
                        //   followed by length bytes, then offset bytes (signed, relative)
                        uint32_t runOffset = attrOffset + nonRes.dataRunsOffset;
                        int64_t prevClusterOffset = 0;

                        while (runOffset < recordData.size())
                        {
                            uint8_t runHeader = recordData[runOffset];
                            if (runHeader == 0)
                                break;

                            uint8_t lenSize = runHeader & 0x0F;
                            uint8_t offSize = (runHeader >> 4) & 0x0F;
                            runOffset++;

                            if (lenSize == 0 || lenSize > 8 || offSize > 8)
                                break;
                            if (runOffset + lenSize + offSize > recordData.size())
                                break;

                            // Read run length (unsigned)
                            uint64_t runLength = 0;
                            std::memcpy(&runLength, &recordData[runOffset], lenSize);
                            runOffset += lenSize;

                            // Read run offset (signed, relative to previous)
                            int64_t runOffsetVal = 0;
                            if (offSize > 0)
                            {
                                std::memcpy(&runOffsetVal, &recordData[runOffset], offSize);
                                // Sign-extend
                                if (recordData[runOffset + offSize - 1] & 0x80)
                                {
                                    for (uint8_t i = offSize; i < 8; ++i)
                                        reinterpret_cast<uint8_t*>(&runOffsetVal)[i] = 0xFF;
                                }
                                runOffset += offSize;
                            }

                            // A zero offset means a sparse run (no actual clusters)
                            if (offSize == 0)
                                continue;

                            prevClusterOffset += runOffsetVal;
                            RecoverableFile::DataRun dr;
                            dr.clusterOffset = static_cast<uint64_t>(prevClusterOffset);
                            dr.clusterCount  = runLength;
                            dataRuns.push_back(dr);
                        }
                    }
                }
                else
                {
                    // Resident $DATA: small file stored entirely in MFT record
                    NtfsResidentAttr resident;
                    if (attrOffset + sizeof(NtfsAttributeHeader) + sizeof(resident) <= recordData.size())
                    {
                        std::memcpy(&resident, &recordData[attrOffset + sizeof(NtfsAttributeHeader)],
                                    sizeof(resident));
                        fileSize = resident.valueLength;
                        // For resident data, we store the data inline; create a single "run"
                        // pointing at the MFT record offset itself
                        RecoverableFile::DataRun dr;
                        dr.clusterOffset = (mftByteOffset + recordIdx * mftRecordSize) / clusterSize;
                        dr.clusterCount  = 1;
                        dataRuns.push_back(dr);
                    }
                }
            }

            attrOffset += attrHeader.length;
        }

        // Skip entries with no name or no data
        if (fileName.empty() || (fileSize == 0 && dataRuns.empty()))
            continue;

        RecoverableFile file;
        file.filename        = fileName;
        file.sizeBytes       = fileSize;
        file.sourceFs        = FilesystemType::NTFS;
        file.confidence      = dataRuns.empty() ? 30.0 : 75.0;
        file.partitionStartLba = m_partStart;
        file.sectorSize      = m_sectorSize;
        file.mftEntryIndex   = recordIdx;
        file.dataRuns        = std::move(dataRuns);

        // Extract extension from filename
        auto dotPos = file.filename.rfind('.');
        if (dotPos != std::string::npos)
            file.extension = file.filename.substr(dotPos + 1);

        results.push_back(std::move(file));

        if (progressCb)
            progressCb(recordIdx, recordsToScan, results.size());
    }

    return results;
}

// ---------------------------------------------------------------------------
// scanFat -- scan FAT directory entries for deleted files
// ---------------------------------------------------------------------------

Result<std::vector<RecoverableFile>> FileRecovery::scanFat(
    FileRecoveryProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    // Read the FAT boot sector
    auto bootResult = readPartitionBytes(0, 512);
    if (bootResult.isError())
        return bootResult.error();

    const auto& boot = bootResult.value();
    if (boot.size() < 512)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "FAT boot sector too small");

    // BPB fields
    uint16_t bytesPerSector = 0;
    uint8_t  sectorsPerCluster = 0;
    uint16_t reservedSectors = 0;
    uint8_t  numberOfFats = 0;
    uint16_t rootEntryCount = 0; // FAT12/16 only (0 for FAT32)
    uint16_t totalSectors16 = 0;
    uint32_t totalSectors32 = 0;
    uint16_t fatSize16 = 0;
    uint32_t fatSize32 = 0;
    uint32_t rootCluster = 0; // FAT32 root directory cluster

    std::memcpy(&bytesPerSector,   &boot[0x0B], 2);
    sectorsPerCluster = boot[0x0D];
    std::memcpy(&reservedSectors,  &boot[0x0E], 2);
    numberOfFats = boot[0x10];
    std::memcpy(&rootEntryCount,   &boot[0x11], 2);
    std::memcpy(&totalSectors16,   &boot[0x13], 2);
    std::memcpy(&fatSize16,        &boot[0x16], 2);
    std::memcpy(&totalSectors32,   &boot[0x20], 4);
    std::memcpy(&fatSize32,        &boot[0x24], 4);
    std::memcpy(&rootCluster,      &boot[0x2C], 4);

    if (bytesPerSector == 0 || sectorsPerCluster == 0 || numberOfFats == 0)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "Invalid FAT BPB");

    uint32_t fatSize = fatSize16 ? fatSize16 : fatSize32;
    uint32_t rootDirSectors = ((rootEntryCount * 32) + (bytesPerSector - 1)) / bytesPerSector;
    uint32_t firstDataSector = reservedSectors + (numberOfFats * fatSize) + rootDirSectors;
    uint32_t clusterSize = static_cast<uint32_t>(sectorsPerCluster) * bytesPerSector;

    const bool isFat32 = (rootEntryCount == 0);

    std::vector<RecoverableFile> results;

    // Helper lambda to scan a block of directory entries
    auto scanDirEntries = [&](uint64_t dirByteOffset, uint32_t dirByteSize)
    {
        auto dirResult = readPartitionBytes(dirByteOffset, dirByteSize);
        if (dirResult.isError())
            return;

        const auto& dirData = dirResult.value();
        uint32_t entryCount = static_cast<uint32_t>(dirData.size()) / 32;

        for (uint32_t i = 0; i < entryCount; ++i)
        {
            if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
                return;

            const uint8_t* entryPtr = &dirData[i * 32];

            // End of directory
            if (entryPtr[0] == 0x00)
                break;

            // Skip long filename entries and volume labels
            if (entryPtr[11] == FAT_ATTR_LONG_NAME)
                continue;
            if (entryPtr[11] & FAT_ATTR_VOLUME_ID)
                continue;

            // Check for deleted marker (0xE5)
            if (entryPtr[0] != FAT_DELETED_MARKER)
                continue;

            FatDirEntry dirEntry;
            std::memcpy(&dirEntry, entryPtr, sizeof(dirEntry));

            // Reconstruct filename from 8.3 format
            std::string name;
            // Base name (8 chars, space-padded)
            for (int j = 0; j < 8; ++j)
            {
                if (dirEntry.name[j] != ' ')
                    name.push_back(static_cast<char>(dirEntry.name[j]));
            }
            // Extension (3 chars)
            std::string ext;
            for (int j = 8; j < 11; ++j)
            {
                if (dirEntry.name[j] != ' ')
                    ext.push_back(static_cast<char>(dirEntry.name[j]));
            }
            if (!ext.empty())
                name += "." + ext;

            // Replace the first character with '_' since we don't know what it was
            // (the deleted marker overwrites the first byte)
            if (!name.empty())
                name[0] = '_';

            uint32_t firstCluster = dirEntry.firstClusterLow;
            if (isFat32)
                firstCluster |= (static_cast<uint32_t>(dirEntry.firstClusterHigh) << 16);

            RecoverableFile file;
            file.filename        = name;
            file.sizeBytes       = dirEntry.fileSize;
            file.sourceFs        = m_fsType;
            file.extension       = ext;
            file.confidence      = 60.0; // Moderate: FAT cluster chains may be overwritten
            file.partitionStartLba = m_partStart;
            file.sectorSize      = m_sectorSize;
            file.firstCluster    = firstCluster;

            // Build data runs by following the FAT chain
            // For deleted files, the FAT entries are typically zeroed, so we can only
            // guarantee the first cluster. Estimate the needed clusters from file size.
            if (firstCluster >= 2 && file.sizeBytes > 0)
            {
                uint32_t clustersNeeded = (static_cast<uint32_t>(file.sizeBytes) + clusterSize - 1) / clusterSize;
                RecoverableFile::DataRun dr;
                dr.clusterOffset = firstCluster;
                dr.clusterCount  = clustersNeeded;
                file.dataRuns.push_back(dr);

                // Higher confidence if the file fits in a contiguous run
                if (clustersNeeded == 1)
                    file.confidence = 85.0;
                else
                    file.confidence = 55.0; // Multi-cluster: may be fragmented
            }

            results.push_back(std::move(file));

            if (progressCb)
                progressCb(i, entryCount, results.size());
        }
    };

    if (isFat32)
    {
        // FAT32: root directory is a cluster chain starting at rootCluster
        // Scan one cluster at a time (simplified: we follow the cluster chain)
        uint32_t currentCluster = rootCluster;
        const uint32_t maxClusters = 4096; // Safety limit
        uint32_t clustersSeen = 0;

        while (currentCluster >= 2 && currentCluster < 0x0FFFFFF8 && clustersSeen < maxClusters)
        {
            uint64_t clusterOffset = static_cast<uint64_t>(firstDataSector) * bytesPerSector +
                                     static_cast<uint64_t>(currentCluster - 2) * clusterSize;
            scanDirEntries(clusterOffset, clusterSize);
            ++clustersSeen;

            // Read the FAT entry for this cluster to find the next one
            uint32_t fatOffset = currentCluster * 4; // FAT32: 4 bytes per entry
            uint64_t fatByteOffset = static_cast<uint64_t>(reservedSectors) * bytesPerSector + fatOffset;
            auto fatResult = readPartitionBytes(fatByteOffset, 4);
            if (fatResult.isError())
                break;

            const auto& fatData = fatResult.value();
            uint32_t nextCluster = 0;
            std::memcpy(&nextCluster, fatData.data(), 4);
            nextCluster &= 0x0FFFFFFF; // Mask off top 4 bits
            currentCluster = nextCluster;
        }
    }
    else
    {
        // FAT12/16: root directory is at a fixed offset
        uint64_t rootDirOffset = static_cast<uint64_t>(reservedSectors + numberOfFats * fatSize)
                                 * bytesPerSector;
        uint32_t rootDirSize = rootEntryCount * 32;
        scanDirEntries(rootDirOffset, rootDirSize);
    }

    // Also scan data area clusters for subdirectory entries
    // (scan first N clusters to find deleted files in subdirectories)
    uint64_t totalSectors = totalSectors16 ? totalSectors16 : totalSectors32;
    uint64_t totalClusters = (totalSectors * bytesPerSector - firstDataSector * bytesPerSector) / clusterSize;
    uint64_t clustersToScan = std::min(totalClusters, static_cast<uint64_t>(8192)); // Cap

    for (uint64_t cluster = 2; cluster < 2 + clustersToScan; ++cluster)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            break;

        uint64_t clusterOffset = static_cast<uint64_t>(firstDataSector) * bytesPerSector +
                                 (cluster - 2) * clusterSize;

        // Quick check: read first 32 bytes and see if it looks like directory entries
        auto peekResult = readPartitionBytes(clusterOffset, 32);
        if (peekResult.isError())
            continue;

        const auto& peek = peekResult.value();
        // Directory clusters often start with "." entry (0x2E) or a deleted entry (0xE5)
        if (peek[0] != 0x2E && peek[0] != FAT_DELETED_MARKER)
            continue;

        scanDirEntries(clusterOffset, clusterSize);
    }

    return results;
}

// ---------------------------------------------------------------------------
// scanExt -- scan ext2/3/4 inodes for deleted files
// ---------------------------------------------------------------------------

Result<std::vector<RecoverableFile>> FileRecovery::scanExt(
    FileRecoveryProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    // Read the ext superblock at offset 1024
    auto sbResult = readPartitionBytes(1024, 1024);
    if (sbResult.isError())
        return sbResult.error();

    const auto& sb = sbResult.value();
    if (sb.size() < 256)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "ext superblock too small");

    // Verify ext magic at superblock offset 0x38 (56)
    uint16_t magic = 0;
    std::memcpy(&magic, &sb[0x38], 2);
    if (magic != EXT_SUPER_MAGIC)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "Not an ext2/3/4 volume");

    // Key superblock fields
    uint32_t inodeCount = 0, blockCount = 0, firstDataBlock = 0;
    uint32_t logBlockSize = 0, inodesPerGroup = 0, blocksPerGroup = 0;
    uint16_t inodeSize = 0;
    uint32_t incompatFeatures = 0;

    std::memcpy(&inodeCount,      &sb[0x00], 4);
    std::memcpy(&blockCount,      &sb[0x04], 4);
    std::memcpy(&firstDataBlock,  &sb[0x14], 4);
    std::memcpy(&logBlockSize,    &sb[0x18], 4);
    std::memcpy(&blocksPerGroup,  &sb[0x20], 4);
    std::memcpy(&inodesPerGroup,  &sb[0x28], 4);
    std::memcpy(&inodeSize,       &sb[0x58], 2);
    std::memcpy(&incompatFeatures, &sb[0x60], 4);

    uint32_t blockSize = 1024u << logBlockSize;
    if (inodeSize == 0)
        inodeSize = 128; // ext2 default

    // Calculate number of block groups
    uint32_t blockGroups = (blockCount + blocksPerGroup - 1) / blocksPerGroup;
    if (blockGroups == 0)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "0 block groups");

    // Block Group Descriptor Table starts at the block after the superblock
    uint64_t bgdtBlock = (firstDataBlock == 0) ? 1 : (firstDataBlock + 1);
    uint64_t bgdtOffset = bgdtBlock * blockSize;

    // Determine BGDT entry size (32 bytes for standard, 64 for 64-bit feature)
    const bool is64bit = (incompatFeatures & 0x80) != 0;
    uint32_t bgdSize = is64bit ? 64 : 32;

    std::vector<RecoverableFile> results;
    uint64_t inodesScanned = 0;

    for (uint32_t bg = 0; bg < blockGroups; ++bg)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled);

        // Read block group descriptor
        uint64_t bgdOffset = bgdtOffset + (bg * bgdSize);
        auto bgdResult = readPartitionBytes(bgdOffset, bgdSize);
        if (bgdResult.isError())
            continue;

        const auto& bgd = bgdResult.value();
        if (bgd.size() < 32)
            continue;

        // Inode table block (offset 8, 4 bytes in BGD)
        uint32_t inodeTableBlock = 0;
        std::memcpy(&inodeTableBlock, &bgd[8], 4);

        // For 64-bit, the high 32 bits are at offset 40
        uint64_t inodeTableBlock64 = inodeTableBlock;
        if (is64bit && bgd.size() >= 44)
        {
            uint32_t hi = 0;
            std::memcpy(&hi, &bgd[40], 4);
            inodeTableBlock64 |= (static_cast<uint64_t>(hi) << 32);
        }

        uint64_t inodeTableOffset = inodeTableBlock64 * blockSize;

        // Read the inode bitmap to identify deleted inodes
        uint32_t inodeBitmapBlock = 0;
        std::memcpy(&inodeBitmapBlock, &bgd[4], 4);
        uint64_t inodeBitmapBlock64 = inodeBitmapBlock;
        if (is64bit && bgd.size() >= 40)
        {
            uint32_t hi = 0;
            std::memcpy(&hi, &bgd[36], 4);
            inodeBitmapBlock64 |= (static_cast<uint64_t>(hi) << 32);
        }

        auto bitmapResult = readPartitionBytes(inodeBitmapBlock64 * blockSize, inodesPerGroup / 8);
        // If bitmap read fails, we'll still scan inodes; just with less filtering
        std::vector<uint8_t> inodeBitmap;
        if (bitmapResult.isOk())
            inodeBitmap = bitmapResult.value();

        // Scan inodes in this block group
        uint32_t inodesInThisGroup = std::min(inodesPerGroup,
                                               inodeCount - (bg * inodesPerGroup));

        // Read the entire inode table for this group (or chunks if too large)
        uint32_t tableSize = inodesInThisGroup * inodeSize;
        const uint32_t chunkSize = 64 * 1024; // 64 KiB chunks

        for (uint32_t chunkStart = 0; chunkStart < tableSize; chunkStart += chunkSize)
        {
            uint32_t thisChunk = std::min(chunkSize, tableSize - chunkStart);
            auto chunkResult = readPartitionBytes(inodeTableOffset + chunkStart, thisChunk);
            if (chunkResult.isError())
                break;

            const auto& chunkData = chunkResult.value();
            uint32_t firstInodeInChunk = chunkStart / inodeSize;
            uint32_t inodesInChunk = thisChunk / inodeSize;

            for (uint32_t localIdx = 0; localIdx < inodesInChunk; ++localIdx)
            {
                uint32_t globalInodeIdx = bg * inodesPerGroup + firstInodeInChunk + localIdx;
                uint32_t inodeNumber = globalInodeIdx + 1; // inodes are 1-based

                // Skip reserved inodes (1-10 in ext2/3, 1-10 in ext4 unless changed)
                if (inodeNumber <= 10)
                    continue;

                // Check bitmap: bit clear = deleted/free
                if (!inodeBitmap.empty())
                {
                    uint32_t bitmapIdx = firstInodeInChunk + localIdx;
                    uint32_t byteIdx = bitmapIdx / 8;
                    uint8_t  bitMask = 1u << (bitmapIdx % 8);
                    if (byteIdx < inodeBitmap.size() && (inodeBitmap[byteIdx] & bitMask))
                        continue; // Inode is in use
                }

                uint32_t inodeOffset = localIdx * inodeSize;
                if (inodeOffset + 128 > chunkData.size())
                    break;

                const uint8_t* inode = &chunkData[inodeOffset];

                // i_mode at offset 0 (2 bytes)
                uint16_t mode = 0;
                std::memcpy(&mode, &inode[0], 2);

                // Skip non-regular files (we want S_IFREG = 0x8000)
                if ((mode & 0xF000) != 0x8000)
                    continue;

                // i_size_lo at offset 4 (4 bytes)
                uint32_t sizeLo = 0;
                std::memcpy(&sizeLo, &inode[4], 4);

                // i_size_high at offset 108 (4 bytes, ext4 only)
                uint32_t sizeHi = 0;
                if (inodeSize >= 128)
                    std::memcpy(&sizeHi, &inode[108], 4);

                uint64_t fileSize = sizeLo | (static_cast<uint64_t>(sizeHi) << 32);

                // Skip empty inodes
                if (fileSize == 0)
                    continue;

                // i_dtime at offset 20 (4 bytes) -- deletion time, non-zero means deleted
                uint32_t dtime = 0;
                std::memcpy(&dtime, &inode[20], 4);

                // For inodes not in bitmap AND with dtime set, they're deleted
                // For ext4, dtime may be 0 if undelete was attempted, so we primarily
                // rely on the bitmap check above.

                // Extract direct block pointers (offset 40, 12 * 4 bytes)
                std::vector<RecoverableFile::DataRun> dataRuns;
                for (int bp = 0; bp < 12; ++bp)
                {
                    uint32_t blockNum = 0;
                    std::memcpy(&blockNum, &inode[40 + bp * 4], 4);
                    if (blockNum == 0)
                        break;

                    // Coalesce contiguous blocks into runs
                    if (!dataRuns.empty() &&
                        dataRuns.back().clusterOffset + dataRuns.back().clusterCount == blockNum)
                    {
                        dataRuns.back().clusterCount++;
                    }
                    else
                    {
                        RecoverableFile::DataRun dr;
                        dr.clusterOffset = blockNum;
                        dr.clusterCount  = 1;
                        dataRuns.push_back(dr);
                    }
                }

                // Check for ext4 extents (i_flags at offset 32, EXT4_EXTENTS_FL = 0x80000)
                uint32_t iFlags = 0;
                std::memcpy(&iFlags, &inode[32], 4);

                if (iFlags & 0x80000) // Uses extents
                {
                    dataRuns.clear();
                    // Extent header at offset 40 in the inode
                    // eh_magic (2 bytes) = 0xF30A, eh_entries (2 bytes)
                    uint16_t ehMagic = 0, ehEntries = 0;
                    std::memcpy(&ehMagic, &inode[40], 2);
                    std::memcpy(&ehEntries, &inode[42], 2);

                    if (ehMagic == 0xF30A)
                    {
                        // Extent entries start at offset 40 + 12 (extent header is 12 bytes)
                        for (uint16_t e = 0; e < ehEntries && e < 4; ++e)
                        {
                            uint32_t extOffset = 40 + 12 + e * 12;
                            if (extOffset + 12 > inodeSize)
                                break;

                            // ee_block (4), ee_len (2), ee_start_hi (2), ee_start_lo (4)
                            uint16_t eeLen = 0;
                            uint16_t eeStartHi = 0;
                            uint32_t eeStartLo = 0;
                            std::memcpy(&eeLen,     &inode[extOffset + 4], 2);
                            std::memcpy(&eeStartHi, &inode[extOffset + 6], 2);
                            std::memcpy(&eeStartLo, &inode[extOffset + 8], 4);

                            uint64_t startBlock = eeStartLo | (static_cast<uint64_t>(eeStartHi) << 32);
                            // ee_len > 32768 means uninitialized extent
                            uint32_t len = (eeLen > 32768) ? (eeLen - 32768) : eeLen;

                            RecoverableFile::DataRun dr;
                            dr.clusterOffset = startBlock;
                            dr.clusterCount  = len;
                            dataRuns.push_back(dr);
                        }
                    }
                }

                RecoverableFile file;
                // We don't have the filename from the inode alone (filenames live in
                // directory entries), so generate a name from the inode number.
                std::ostringstream oss;
                oss << "inode_" << inodeNumber;
                file.filename        = oss.str();
                file.sizeBytes       = fileSize;
                file.sourceFs        = m_fsType;
                file.confidence      = dataRuns.empty() ? 25.0 : 65.0;
                file.partitionStartLba = m_partStart;
                file.sectorSize      = m_sectorSize;
                file.inodeNumber     = inodeNumber;
                file.dataRuns        = std::move(dataRuns);

                results.push_back(std::move(file));
                ++inodesScanned;

                if (progressCb && (inodesScanned % 1000 == 0))
                    progressCb(inodesScanned, inodeCount, results.size());
            }
        }
    }

    return results;
}

// ---------------------------------------------------------------------------
// scanCarving -- raw sector scan for known file headers
// ---------------------------------------------------------------------------

std::vector<CarvedFileSignature> FileRecovery::getDefaultSignatures()
{
    return {
        // JPEG: FFD8FF
        {{0xFF, 0xD8, 0xFF}, 0, "jpg", "JPEG Image", {0xFF, 0xD9}, 50 * 1024 * 1024},
        // PNG: 89504E47 0D0A1A0A
        {{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}, 0, "png", "PNG Image",
         {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82}, 100 * 1024 * 1024},
        // PDF: %PDF (25504446)
        {{0x25, 0x50, 0x44, 0x46}, 0, "pdf", "PDF Document", {}, 500 * 1024 * 1024},
        // ZIP/DOCX/XLSX/PPTX: PK (504B0304)
        {{0x50, 0x4B, 0x03, 0x04}, 0, "zip", "ZIP Archive", {}, 2ULL * 1024 * 1024 * 1024},
        // MP4/MOV: ftyp at offset 4
        {{0x66, 0x74, 0x79, 0x70}, 4, "mp4", "MP4 Video", {}, 4ULL * 1024 * 1024 * 1024},
        // GIF: GIF89a or GIF87a
        {{0x47, 0x49, 0x46, 0x38, 0x39, 0x61}, 0, "gif", "GIF Image",
         {0x00, 0x3B}, 50 * 1024 * 1024},
        // BMP: BM
        {{0x42, 0x4D}, 0, "bmp", "BMP Image", {}, 100 * 1024 * 1024},
        // RAR: Rar! (526172211A07)
        {{0x52, 0x61, 0x72, 0x21, 0x1A, 0x07}, 0, "rar", "RAR Archive",
         {}, 2ULL * 1024 * 1024 * 1024},
        // 7z: 377ABCAF271C
        {{0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C}, 0, "7z", "7-Zip Archive",
         {}, 2ULL * 1024 * 1024 * 1024},
        // TIFF (little-endian): II (4949 2A00)
        {{0x49, 0x49, 0x2A, 0x00}, 0, "tif", "TIFF Image", {}, 500 * 1024 * 1024},
        // TIFF (big-endian): MM (4D4D 002A)
        {{0x4D, 0x4D, 0x00, 0x2A}, 0, "tif", "TIFF Image (BE)", {}, 500 * 1024 * 1024},
        // EXE/DLL: MZ (4D5A)
        {{0x4D, 0x5A}, 0, "exe", "Windows Executable", {}, 500 * 1024 * 1024},
        // SQLite: SQLite format 3 (53514C69746520666F726D61742033)
        {{0x53, 0x51, 0x4C, 0x69, 0x74, 0x65, 0x20, 0x66,
          0x6F, 0x72, 0x6D, 0x61, 0x74, 0x20, 0x33}, 0,
         "sqlite", "SQLite Database", {}, 2ULL * 1024 * 1024 * 1024},
    };
}

Result<std::vector<RecoverableFile>> FileRecovery::scanCarving(
    FileRecoveryProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    const auto signatures = getDefaultSignatures();
    std::vector<RecoverableFile> results;

    // Compute the maximum header length + offset we need to check
    uint32_t maxHeaderCheck = 0;
    for (const auto& sig : signatures)
    {
        uint32_t needed = sig.headerOffset + static_cast<uint32_t>(sig.header.size());
        if (needed > maxHeaderCheck)
            maxHeaderCheck = needed;
    }

    // Read in 64 KiB chunks, checking every sector-aligned offset
    const uint32_t chunkSectors = 128; // 128 * 512 = 64 KiB
    const uint64_t totalSectors = m_partSectors;
    uint32_t carvedCount = 0;

    for (uint64_t sectorIdx = 0; sectorIdx < totalSectors; sectorIdx += chunkSectors)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled);

        uint64_t sectorsRemaining = totalSectors - sectorIdx;
        uint64_t sectorsToRead = std::min(static_cast<uint64_t>(chunkSectors), sectorsRemaining);

        auto chunkResult = readPartitionBytes(sectorIdx * m_sectorSize,
                                               static_cast<uint32_t>(sectorsToRead * m_sectorSize));
        if (chunkResult.isError())
            continue;

        const auto& chunk = chunkResult.value();

        // Check every sector boundary within this chunk
        for (uint64_t off = 0; off + maxHeaderCheck <= chunk.size(); off += m_sectorSize)
        {
            for (const auto& sig : signatures)
            {
                uint64_t headerStart = off + sig.headerOffset;
                if (headerStart + sig.header.size() > chunk.size())
                    continue;

                if (std::memcmp(&chunk[headerStart], sig.header.data(), sig.header.size()) == 0)
                {
                    RecoverableFile file;
                    std::ostringstream oss;
                    oss << "carved_" << std::setw(6) << std::setfill('0') << carvedCount
                        << "." << sig.extension;
                    file.filename   = oss.str();
                    file.extension  = sig.extension;
                    file.sourceFs   = FilesystemType::Raw;
                    file.sizeBytes  = sig.maxSize; // Upper bound; actual may be smaller
                    file.confidence = 50.0;
                    file.partitionStartLba = m_partStart;
                    file.sectorSize = m_sectorSize;
                    file.carvedLba  = m_partStart + sectorIdx + (off / m_sectorSize);

                    results.push_back(std::move(file));
                    ++carvedCount;
                }
            }
        }

        if (progressCb)
            progressCb(sectorIdx + sectorsToRead, totalSectors, results.size());
    }

    return results;
}

// ---------------------------------------------------------------------------
// recoverFile -- recover a file to an output path
// ---------------------------------------------------------------------------

Result<void> FileRecovery::recoverFile(const RecoverableFile& file,
                                        const std::string& outputPath)
{
    switch (file.sourceFs)
    {
    case FilesystemType::NTFS:
        return recoverNtfsFile(file, outputPath);
    case FilesystemType::FAT12:
    case FilesystemType::FAT16:
    case FilesystemType::FAT32:
        return recoverFatFile(file, outputPath);
    case FilesystemType::Ext2:
    case FilesystemType::Ext3:
    case FilesystemType::Ext4:
        return recoverExtFile(file, outputPath);
    case FilesystemType::Raw:
        return recoverCarvedFile(file, outputPath);
    default:
        return ErrorInfo::fromCode(ErrorCode::FilesystemNotSupported,
                                   "Cannot recover from this filesystem type");
    }
}

// ---------------------------------------------------------------------------
// recoverNtfsFile -- read data runs and assemble the file
// ---------------------------------------------------------------------------

Result<void> FileRecovery::recoverNtfsFile(const RecoverableFile& file,
                                            const std::string& outputPath)
{
    // Read boot sector to get cluster size
    auto bootResult = readPartitionBytes(0, 512);
    if (bootResult.isError())
        return bootResult.error();

    const auto& boot = bootResult.value();
    uint16_t bytesPerSector = 0;
    uint8_t sectorsPerCluster = 0;
    std::memcpy(&bytesPerSector, &boot[0x0B], 2);
    sectorsPerCluster = boot[0x0D];
    if (bytesPerSector == 0 || sectorsPerCluster == 0)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "Invalid NTFS BPB");

    uint32_t clusterSize = static_cast<uint32_t>(bytesPerSector) * sectorsPerCluster;

    std::ofstream outFile(outputPath, std::ios::binary | std::ios::trunc);
    if (!outFile.is_open())
        return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
                                   "Cannot create output file: " + outputPath);

    uint64_t bytesWritten = 0;
    for (const auto& run : file.dataRuns)
    {
        uint64_t runByteOffset = run.clusterOffset * clusterSize;
        uint64_t runByteSize   = run.clusterCount * clusterSize;

        // Read in 1 MiB chunks
        const uint32_t readChunk = 1024 * 1024;
        for (uint64_t off = 0; off < runByteSize; off += readChunk)
        {
            uint32_t toRead = static_cast<uint32_t>(std::min(
                static_cast<uint64_t>(readChunk), runByteSize - off));

            auto dataResult = readPartitionBytes(runByteOffset + off, toRead);
            if (dataResult.isError())
                return dataResult.error();

            const auto& data = dataResult.value();
            uint64_t toWrite = data.size();
            // Don't exceed the known file size
            if (file.sizeBytes > 0 && bytesWritten + toWrite > file.sizeBytes)
                toWrite = file.sizeBytes - bytesWritten;

            if (toWrite > 0)
            {
                outFile.write(reinterpret_cast<const char*>(data.data()),
                              static_cast<std::streamsize>(toWrite));
                bytesWritten += toWrite;
            }

            if (file.sizeBytes > 0 && bytesWritten >= file.sizeBytes)
                break;
        }

        if (file.sizeBytes > 0 && bytesWritten >= file.sizeBytes)
            break;
    }

    outFile.close();
    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// recoverFatFile -- read clusters assuming contiguous allocation
// ---------------------------------------------------------------------------

Result<void> FileRecovery::recoverFatFile(const RecoverableFile& file,
                                           const std::string& outputPath)
{
    // Re-read FAT BPB to compute cluster geometry
    auto bootResult = readPartitionBytes(0, 512);
    if (bootResult.isError())
        return bootResult.error();

    const auto& boot = bootResult.value();
    uint16_t bytesPerSector = 0;
    uint8_t sectorsPerCluster = 0;
    uint16_t reservedSectors = 0;
    uint8_t numberOfFats = 0;
    uint16_t rootEntryCount = 0;
    uint16_t fatSize16 = 0;
    uint32_t fatSize32 = 0;

    std::memcpy(&bytesPerSector,  &boot[0x0B], 2);
    sectorsPerCluster = boot[0x0D];
    std::memcpy(&reservedSectors, &boot[0x0E], 2);
    numberOfFats = boot[0x10];
    std::memcpy(&rootEntryCount,  &boot[0x11], 2);
    std::memcpy(&fatSize16,       &boot[0x16], 2);
    std::memcpy(&fatSize32,       &boot[0x24], 4);

    if (bytesPerSector == 0 || sectorsPerCluster == 0)
        return ErrorInfo::fromCode(ErrorCode::FilesystemCorrupt, "Invalid FAT BPB");

    uint32_t fatSize = fatSize16 ? fatSize16 : fatSize32;
    uint32_t rootDirSectors = ((rootEntryCount * 32) + (bytesPerSector - 1)) / bytesPerSector;
    uint32_t firstDataSector = reservedSectors + (numberOfFats * fatSize) + rootDirSectors;
    uint32_t clusterSize = static_cast<uint32_t>(sectorsPerCluster) * bytesPerSector;

    std::ofstream outFile(outputPath, std::ios::binary | std::ios::trunc);
    if (!outFile.is_open())
        return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
                                   "Cannot create output file: " + outputPath);

    uint64_t bytesWritten = 0;
    for (const auto& run : file.dataRuns)
    {
        // Convert cluster number to byte offset
        uint64_t clusterByteOffset =
            static_cast<uint64_t>(firstDataSector) * bytesPerSector +
            (run.clusterOffset - 2) * clusterSize;
        uint64_t runByteSize = run.clusterCount * clusterSize;

        const uint32_t readChunk = 1024 * 1024;
        for (uint64_t off = 0; off < runByteSize; off += readChunk)
        {
            uint32_t toRead = static_cast<uint32_t>(std::min(
                static_cast<uint64_t>(readChunk), runByteSize - off));

            auto dataResult = readPartitionBytes(clusterByteOffset + off, toRead);
            if (dataResult.isError())
                return dataResult.error();

            const auto& data = dataResult.value();
            uint64_t toWrite = data.size();
            if (file.sizeBytes > 0 && bytesWritten + toWrite > file.sizeBytes)
                toWrite = file.sizeBytes - bytesWritten;

            if (toWrite > 0)
            {
                outFile.write(reinterpret_cast<const char*>(data.data()),
                              static_cast<std::streamsize>(toWrite));
                bytesWritten += toWrite;
            }

            if (file.sizeBytes > 0 && bytesWritten >= file.sizeBytes)
                break;
        }
    }

    outFile.close();
    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// recoverExtFile -- read blocks referenced by data runs
// ---------------------------------------------------------------------------

Result<void> FileRecovery::recoverExtFile(const RecoverableFile& file,
                                           const std::string& outputPath)
{
    // Read ext superblock to get block size
    auto sbResult = readPartitionBytes(1024, 256);
    if (sbResult.isError())
        return sbResult.error();

    const auto& sb = sbResult.value();
    uint32_t logBlockSize = 0;
    std::memcpy(&logBlockSize, &sb[0x18], 4);
    uint32_t blockSize = 1024u << logBlockSize;

    std::ofstream outFile(outputPath, std::ios::binary | std::ios::trunc);
    if (!outFile.is_open())
        return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
                                   "Cannot create output file: " + outputPath);

    uint64_t bytesWritten = 0;
    for (const auto& run : file.dataRuns)
    {
        uint64_t blockByteOffset = run.clusterOffset * blockSize;
        uint64_t runByteSize     = run.clusterCount * blockSize;

        const uint32_t readChunk = 1024 * 1024;
        for (uint64_t off = 0; off < runByteSize; off += readChunk)
        {
            uint32_t toRead = static_cast<uint32_t>(std::min(
                static_cast<uint64_t>(readChunk), runByteSize - off));

            auto dataResult = readPartitionBytes(blockByteOffset + off, toRead);
            if (dataResult.isError())
                return dataResult.error();

            const auto& data = dataResult.value();
            uint64_t toWrite = data.size();
            if (file.sizeBytes > 0 && bytesWritten + toWrite > file.sizeBytes)
                toWrite = file.sizeBytes - bytesWritten;

            if (toWrite > 0)
            {
                outFile.write(reinterpret_cast<const char*>(data.data()),
                              static_cast<std::streamsize>(toWrite));
                bytesWritten += toWrite;
            }

            if (file.sizeBytes > 0 && bytesWritten >= file.sizeBytes)
                break;
        }
    }

    outFile.close();
    return Result<void>::ok();
}

// ---------------------------------------------------------------------------
// recoverCarvedFile -- read raw sectors starting from carved LBA
// ---------------------------------------------------------------------------

Result<void> FileRecovery::recoverCarvedFile(const RecoverableFile& file,
                                              const std::string& outputPath)
{
    std::ofstream outFile(outputPath, std::ios::binary | std::ios::trunc);
    if (!outFile.is_open())
        return ErrorInfo::fromCode(ErrorCode::FileCreateFailed,
                                   "Cannot create output file: " + outputPath);

    // Read from the carved LBA, up to maxSize.
    // For types with known footers, we scan for the footer.
    // Otherwise we just read maxSize bytes.
    const auto signatures = getDefaultSignatures();

    // Find the matching signature for this file
    std::vector<uint8_t> footer;
    uint64_t maxSize = file.sizeBytes;
    for (const auto& sig : signatures)
    {
        if (sig.extension == file.extension)
        {
            footer = sig.footer;
            if (maxSize == 0 || maxSize > sig.maxSize)
                maxSize = sig.maxSize;
            break;
        }
    }

    if (maxSize == 0)
        maxSize = 10 * 1024 * 1024; // Default 10 MiB cap for unknown types

    // Read sectors from the absolute carved position
    // file.carvedLba is already absolute (partition start + offset)
    uint64_t bytesWritten = 0;
    const uint32_t readChunk = 256 * 1024; // 256 KiB chunks

    while (bytesWritten < maxSize)
    {
        uint32_t toRead = static_cast<uint32_t>(std::min(
            static_cast<uint64_t>(readChunk), maxSize - bytesWritten));

        SectorOffset startSector = file.carvedLba + (bytesWritten / m_sectorSize);
        SectorCount sectorsToRead = (toRead + m_sectorSize - 1) / m_sectorSize;

        auto readResult = m_disk.readSectors(startSector, sectorsToRead, m_sectorSize);
        if (readResult.isError())
            break;

        const auto& data = readResult.value();

        // If we have a footer, search for it in this chunk
        if (!footer.empty())
        {
            for (size_t i = 0; i + footer.size() <= data.size(); ++i)
            {
                if (std::memcmp(&data[i], footer.data(), footer.size()) == 0)
                {
                    // Found footer; write up to and including the footer
                    uint64_t finalSize = i + footer.size();
                    outFile.write(reinterpret_cast<const char*>(data.data()),
                                  static_cast<std::streamsize>(finalSize));
                    outFile.close();
                    return Result<void>::ok();
                }
            }
        }

        // No footer found (or no footer defined); write the whole chunk
        uint64_t toWrite = std::min(static_cast<uint64_t>(data.size()), maxSize - bytesWritten);
        outFile.write(reinterpret_cast<const char*>(data.data()),
                      static_cast<std::streamsize>(toWrite));
        bytesWritten += toWrite;
    }

    outFile.close();
    return Result<void>::ok();
}

} // namespace spw
