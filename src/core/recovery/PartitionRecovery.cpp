// PartitionRecovery.cpp -- Scan for lost/deleted partition superblocks.
//
// DISCLAIMER: This code is for authorized disk utility software only.

#include "PartitionRecovery.h"

#include <algorithm>
#include <cstring>

namespace spw
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PartitionRecovery::PartitionRecovery(RawDiskHandle& disk)
    : m_disk(disk)
{
}

// ---------------------------------------------------------------------------
// scan -- iterate over the disk looking for filesystem signatures
// ---------------------------------------------------------------------------

Result<std::vector<RecoveredPartition>> PartitionRecovery::scan(
    PartitionScanMode mode,
    PartitionScanProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    // Fetch disk geometry so we know how many sectors to scan
    auto geoResult = m_disk.getGeometry();
    if (geoResult.isError())
        return geoResult.error();
    m_geometry = geoResult.value();

    // Fetch existing partition layout for overlap detection
    auto layoutResult = m_disk.getDriveLayout();
    if (layoutResult.isOk())
        m_layout = layoutResult.value();
    // Failure is non-fatal: we simply won't mark overlaps

    const uint32_t sectorSize = m_geometry.bytesPerSector;
    if (sectorSize == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Disk reports 0 bytes/sector");

    const uint64_t totalSectors = m_geometry.totalBytes / sectorSize;
    if (totalSectors == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Disk reports 0 total sectors");

    // Calculate step size.
    // Quick mode: 1 MiB boundaries (DEFAULT_ALIGNMENT_BYTES / sectorSize).
    // Deep mode:  every single sector.
    const uint64_t stepSectors = (mode == PartitionScanMode::Quick)
        ? (DEFAULT_ALIGNMENT_BYTES / sectorSize)
        : 1;

    // We also probe old-school cylinder boundaries (63 sectors, 2048 sectors)
    // during quick scans, since pre-Vista partitions commonly started on
    // cylinder boundaries rather than 1 MiB boundaries.
    constexpr uint64_t LEGACY_CHS_STEP = 63;   // sectors per track on classic BIOS disks

    std::vector<RecoveredPartition> results;

    uint64_t scannedSectors = 0;
    for (uint64_t lba = 0; lba < totalSectors; lba += stepSectors)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled, "Partition scan canceled");

        RecoveredPartition candidate;
        if (probeOffset(lba, candidate))
        {
            candidate.sectorSize = sectorSize;
            results.push_back(candidate);
        }

        scannedSectors += stepSectors;
        if (progressCb)
            progressCb(std::min(scannedSectors, totalSectors), totalSectors, results.size());
    }

    // Quick scan: also probe legacy cylinder boundaries that aren't on 1 MiB multiples
    if (mode == PartitionScanMode::Quick)
    {
        for (uint64_t lba = LEGACY_CHS_STEP; lba < totalSectors; lba += LEGACY_CHS_STEP)
        {
            // Skip if this LBA was already covered by the 1 MiB pass
            if ((lba * sectorSize) % DEFAULT_ALIGNMENT_BYTES == 0)
                continue;

            if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
                break;

            RecoveredPartition candidate;
            if (probeOffset(lba, candidate))
            {
                candidate.sectorSize = sectorSize;
                results.push_back(candidate);
            }
        }
    }

    // Mark partitions that overlap existing entries
    markOverlaps(results);

    if (results.empty())
        return ErrorInfo::fromCode(ErrorCode::NoPartitionsFound, "No lost partitions found");

    return results;
}

// ---------------------------------------------------------------------------
// probeOffset -- try to identify a filesystem superblock at the given LBA
// ---------------------------------------------------------------------------

bool PartitionRecovery::probeOffset(SectorOffset lba, RecoveredPartition& out) const
{
    const uint32_t sectorSize = m_geometry.bytesPerSector;
    const uint64_t byteOffset = lba * sectorSize;

    // We need to read enough data to detect any filesystem.
    // Most signatures are in the first 4 KiB, but ext superblock is at offset
    // 1024 from partition start and Btrfs superblock is at 0x10000 (64 KiB).
    // Read the first sector first (cheap), then extend if needed.

    // Create a read callback rooted at this LBA for FilesystemDetector
    auto readFunc = [this, byteOffset, sectorSize](uint64_t offset, uint32_t size) -> Result<std::vector<uint8_t>>
    {
        // Convert the relative offset to an absolute sector address
        uint64_t absOffset = byteOffset + offset;
        SectorOffset startSector = absOffset / sectorSize;
        // Round size up to sector boundary
        uint32_t alignedSize = ((size + sectorSize - 1) / sectorSize) * sectorSize;
        SectorCount sectorsToRead = alignedSize / sectorSize;

        auto readResult = m_disk.readSectors(startSector, sectorsToRead, sectorSize);
        if (readResult.isError())
            return readResult.error();

        // Trim to the requested sub-range
        auto& data = readResult.value();
        uint32_t inSectorOffset = static_cast<uint32_t>(absOffset % sectorSize);
        if (inSectorOffset + size > data.size())
            return ErrorInfo::fromCode(ErrorCode::DiskReadError, "Read underflow");

        std::vector<uint8_t> trimmed(data.begin() + inSectorOffset,
                                      data.begin() + inSectorOffset + size);
        return trimmed;
    };

    auto detectResult = FilesystemDetector::detect(readFunc, 0);
    if (detectResult.isError() || !detectResult.value().isDetected())
        return false;

    const auto& detection = detectResult.value();
    out.startLba = lba;
    out.fsType   = detection.type;
    out.label    = detection.label;

    // Estimate partition size from the superblock
    uint64_t estimatedBytes = estimatePartitionSize(lba, detection.type);
    if (estimatedBytes > 0)
    {
        out.sectorCount = estimatedBytes / sectorSize;
    }
    else
    {
        // Fallback: unknown size, mark it as spanning to next found partition
        // or end of disk.  Set to 0 and let the caller decide.
        out.sectorCount = 0;
    }

    // Confidence heuristic:
    //  - Known modern FS at 1 MiB boundary -> 95%
    //  - Known modern FS at cylinder boundary -> 85%
    //  - Known modern FS at other offset -> 70%
    //  - Exotic/unknown FS -> 50%
    const bool onMibBoundary = ((lba * sectorSize) % DEFAULT_ALIGNMENT_BYTES == 0) && (lba != 0);
    const bool onCylBoundary = (lba % 63 == 0) && (lba != 0);
    const bool isModernFs = (detection.type == FilesystemType::NTFS  ||
                              detection.type == FilesystemType::FAT32 ||
                              detection.type == FilesystemType::FAT16 ||
                              detection.type == FilesystemType::ExFAT ||
                              detection.type == FilesystemType::Ext4  ||
                              detection.type == FilesystemType::Ext3  ||
                              detection.type == FilesystemType::Ext2  ||
                              detection.type == FilesystemType::Btrfs ||
                              detection.type == FilesystemType::XFS);

    if (isModernFs && onMibBoundary)      out.confidence = 95.0;
    else if (isModernFs && onCylBoundary) out.confidence = 85.0;
    else if (isModernFs)                  out.confidence = 70.0;
    else if (lba == 0)                    out.confidence = 30.0; // Sector 0 is usually MBR/GPT
    else                                  out.confidence = 50.0;

    return true;
}

// ---------------------------------------------------------------------------
// estimatePartitionSize -- read the superblock to extract volume size
// ---------------------------------------------------------------------------

uint64_t PartitionRecovery::estimatePartitionSize(SectorOffset lba, FilesystemType fs) const
{
    const uint32_t sectorSize = m_geometry.bytesPerSector;
    const uint64_t byteOffset = lba * sectorSize;

    // For each filesystem type, we know where the volume-size field lives in
    // the superblock.  Read the relevant bytes and extract the value.

    auto readAbsolute = [this, sectorSize](uint64_t absOffset, uint32_t size)
        -> std::vector<uint8_t>
    {
        SectorOffset startSector = absOffset / sectorSize;
        uint32_t alignedSize = ((size + sectorSize - 1) / sectorSize) * sectorSize;
        SectorCount sectorsToRead = alignedSize / sectorSize;
        auto result = m_disk.readSectors(startSector, sectorsToRead, sectorSize);
        if (result.isError())
            return {};
        auto& data = result.value();
        uint32_t inOffset = static_cast<uint32_t>(absOffset % sectorSize);
        if (inOffset + size > data.size())
            return {};
        return std::vector<uint8_t>(data.begin() + inOffset, data.begin() + inOffset + size);
    };

    switch (fs)
    {
    case FilesystemType::NTFS:
    {
        // NTFS BPB: total sectors at offset 0x28 (8 bytes, little-endian)
        auto bpb = readAbsolute(byteOffset, 512);
        if (bpb.size() < 0x30)
            return 0;
        uint64_t totalSectors = 0;
        std::memcpy(&totalSectors, &bpb[0x28], 8);
        return totalSectors * sectorSize;
    }
    case FilesystemType::FAT32:
    case FilesystemType::FAT16:
    case FilesystemType::FAT12:
    {
        // FAT BPB: total sectors 16 at offset 0x13 (2 bytes), total sectors 32 at 0x20 (4 bytes)
        auto bpb = readAbsolute(byteOffset, 512);
        if (bpb.size() < 0x24)
            return 0;
        uint16_t totalSectors16 = 0;
        uint32_t totalSectors32 = 0;
        std::memcpy(&totalSectors16, &bpb[0x13], 2);
        std::memcpy(&totalSectors32, &bpb[0x20], 4);
        uint64_t totalSectors = totalSectors16 ? totalSectors16 : totalSectors32;
        // Bytes per sector from BPB
        uint16_t bps = 0;
        std::memcpy(&bps, &bpb[0x0B], 2);
        if (bps == 0)
            bps = static_cast<uint16_t>(sectorSize);
        return totalSectors * bps;
    }
    case FilesystemType::ExFAT:
    {
        // exFAT: volume length at offset 0x48 (8 bytes, sectors)
        auto boot = readAbsolute(byteOffset, 512);
        if (boot.size() < 0x50)
            return 0;
        uint64_t volumeLength = 0;
        std::memcpy(&volumeLength, &boot[0x48], 8);
        // exFAT sector size is 2^(BytesPerSectorShift) at offset 0x6C
        uint8_t bpsShift = boot[0x6C];
        uint32_t exfatSectorSize = (bpsShift > 0 && bpsShift <= 12) ? (1u << bpsShift) : sectorSize;
        return volumeLength * exfatSectorSize;
    }
    case FilesystemType::Ext2:
    case FilesystemType::Ext3:
    case FilesystemType::Ext4:
    {
        // ext superblock at offset 1024 from partition start.
        // s_blocks_count_lo at offset 4 (4 bytes), s_log_block_size at offset 24 (4 bytes).
        // For ext4 with 64-bit feature, s_blocks_count_hi at offset 0x150 (4 bytes).
        auto sb = readAbsolute(byteOffset + 1024, 512);
        if (sb.size() < 256)
            return 0;

        uint32_t blocksLo = 0, logBlockSize = 0;
        std::memcpy(&blocksLo, &sb[4], 4);
        std::memcpy(&logBlockSize, &sb[24], 4);

        uint64_t blockSize = 1024ULL << logBlockSize;
        uint64_t totalBlocks = blocksLo;

        // Check for 64-bit block count (ext4 feature flag at offset 0x60, bit 0x80 = INCOMPAT_64BIT)
        if (sb.size() >= 0x154)
        {
            uint32_t incompatFeatures = 0;
            std::memcpy(&incompatFeatures, &sb[0x60], 4);
            if (incompatFeatures & 0x80) // INCOMPAT_64BIT
            {
                uint32_t blocksHi = 0;
                std::memcpy(&blocksHi, &sb[0x150 - 1024 + 1024], 4); // offset 0x150 in superblock
                // Superblock starts at partition+1024, so offset within our 512-byte read at
                // partition+1024 is relative.  We need to re-read if sb isn't large enough.
                // Simpler: read a larger chunk.
                auto sbFull = readAbsolute(byteOffset + 1024, 1024);
                if (sbFull.size() >= 0x154)
                {
                    std::memcpy(&blocksHi, &sbFull[0x150 - 1024 + 1024 - 1024], 4);
                    // Offset 0x150 in the superblock.  Our buffer starts at superblock offset 0.
                    // So it's at buffer[0x150].  But we only read 1024 bytes -> 0x150 = 336, within range.
                    std::memcpy(&blocksHi, &sbFull[0x150], 4);
                    totalBlocks |= (static_cast<uint64_t>(blocksHi) << 32);
                }
            }
        }

        return totalBlocks * blockSize;
    }
    case FilesystemType::Btrfs:
    {
        // Btrfs superblock at 0x10000 from partition start.
        // total_bytes at offset 0x70 (8 bytes) within the superblock.
        auto sb = readAbsolute(byteOffset + 0x10000, 256);
        if (sb.size() < 0x78)
            return 0;
        uint64_t totalBytes = 0;
        std::memcpy(&totalBytes, &sb[0x70], 8);
        return totalBytes;
    }
    case FilesystemType::XFS:
    {
        // XFS superblock at partition start.
        // sb_dblocks (total data blocks) at offset 8 (8 bytes, big-endian).
        // sb_blocksize at offset 4 (4 bytes, big-endian).
        auto sb = readAbsolute(byteOffset, 512);
        if (sb.size() < 20)
            return 0;

        // XFS is big-endian on disk
        uint32_t blockSizeBE = 0;
        uint64_t totalBlocksBE = 0;
        std::memcpy(&blockSizeBE, &sb[4], 4);
        std::memcpy(&totalBlocksBE, &sb[8], 8);

        // Byte-swap from big-endian
        uint32_t xfsBlockSize =
            ((blockSizeBE >> 24) & 0xFF) |
            ((blockSizeBE >>  8) & 0xFF00) |
            ((blockSizeBE <<  8) & 0xFF0000) |
            ((blockSizeBE << 24) & 0xFF000000);

        uint64_t xfsTotalBlocks =
            ((totalBlocksBE >> 56) & 0xFF) |
            ((totalBlocksBE >> 40) & 0xFF00) |
            ((totalBlocksBE >> 24) & 0xFF0000) |
            ((totalBlocksBE >>  8) & 0xFF000000ULL) |
            ((totalBlocksBE <<  8) & 0xFF00000000ULL) |
            ((totalBlocksBE << 24) & 0xFF0000000000ULL) |
            ((totalBlocksBE << 40) & 0xFF000000000000ULL) |
            ((totalBlocksBE << 56) & 0xFF00000000000000ULL);

        return xfsTotalBlocks * xfsBlockSize;
    }
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// markOverlaps -- flag found partitions that overlap current table entries
// ---------------------------------------------------------------------------

void PartitionRecovery::markOverlaps(std::vector<RecoveredPartition>& results) const
{
    for (auto& found : results)
    {
        if (found.sectorCount == 0)
            continue;

        uint64_t foundStart = found.startLba;
        uint64_t foundEnd   = found.startLba + found.sectorCount;

        for (const auto& existing : m_layout.partitions)
        {
            uint64_t existStart = existing.startingOffset / m_geometry.bytesPerSector;
            uint64_t existEnd   = existStart + (existing.partitionLength / m_geometry.bytesPerSector);

            // Classic overlap test: A.start < B.end && B.start < A.end
            if (foundStart < existEnd && existStart < foundEnd)
            {
                found.overlapsExisting = true;

                // If it exactly matches an existing partition, lower confidence
                // significantly because it's not actually "lost"
                if (foundStart == existStart && foundEnd == existEnd)
                    found.confidence = 10.0;

                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// recover -- write a found partition back to the on-disk partition table
// ---------------------------------------------------------------------------

Result<void> PartitionRecovery::recover(const RecoveredPartition& partition)
{
    if (partition.sectorCount == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
                                   "Cannot recover partition with unknown size");

    const uint32_t sectorSize = m_geometry.bytesPerSector;

    // Build a DiskReadCallback so we can parse the existing table
    auto readFunc = [this, sectorSize](uint64_t offset, uint32_t size) -> Result<std::vector<uint8_t>>
    {
        SectorOffset startSector = offset / sectorSize;
        uint32_t aligned = ((size + sectorSize - 1) / sectorSize) * sectorSize;
        return m_disk.readSectors(startSector, aligned / sectorSize, sectorSize);
    };

    auto tableResult = PartitionTable::parse(readFunc, m_geometry.totalBytes, sectorSize);
    if (tableResult.isError())
        return tableResult.error();

    auto& table = tableResult.value();

    // Build a PartitionParams for the new entry
    PartitionParams params;
    params.startLba    = partition.startLba;
    params.sectorCount = partition.sectorCount;

    if (table->type() == PartitionTableType::MBR)
    {
        // Determine MBR type byte from filesystem type
        switch (partition.fsType)
        {
        case FilesystemType::NTFS:
        case FilesystemType::ExFAT:
            params.mbrType = MbrTypes::NTFS_HPFS;
            break;
        case FilesystemType::FAT32:
            params.mbrType = MbrTypes::FAT32_LBA;
            break;
        case FilesystemType::FAT16:
            params.mbrType = MbrTypes::FAT16_LBA;
            break;
        case FilesystemType::FAT12:
            params.mbrType = MbrTypes::FAT12;
            break;
        case FilesystemType::Ext2:
        case FilesystemType::Ext3:
        case FilesystemType::Ext4:
        case FilesystemType::Btrfs:
        case FilesystemType::XFS:
            params.mbrType = MbrTypes::LinuxNative;
            break;
        default:
            params.mbrType = MbrTypes::NTFS_HPFS; // Safe default for data partitions
            break;
        }
    }
    else if (table->type() == PartitionTableType::GPT)
    {
        // Use Microsoft Basic Data GUID as default; adjust for Linux filesystems
        switch (partition.fsType)
        {
        case FilesystemType::Ext2:
        case FilesystemType::Ext3:
        case FilesystemType::Ext4:
        case FilesystemType::Btrfs:
        case FilesystemType::XFS:
            params.typeGuid = GptTypes::linuxFilesystem();
            break;
        default:
            params.typeGuid = GptTypes::microsoftBasicData();
            break;
        }
        params.gptName = partition.label.empty() ? "Recovered Partition" : partition.label;
    }

    auto addResult = table->addPartition(params);
    if (addResult.isError())
        return addResult;

    // Serialize the modified table to bytes and write it back to disk
    auto serResult = table->serialize();
    if (serResult.isError())
        return serResult.error();

    const auto& tableBytes = serResult.value();
    // Write sector 0 (and additional sectors for GPT)
    SectorCount tableSectors = (tableBytes.size() + sectorSize - 1) / sectorSize;
    auto writeResult = m_disk.writeSectors(0, tableBytes.data(), tableSectors, sectorSize);
    if (writeResult.isError())
        return writeResult;

    return Result<void>::ok();
}

} // namespace spw
