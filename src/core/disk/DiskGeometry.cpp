#include "DiskGeometry.h"

namespace spw
{
namespace DiskGeometry
{

Result<SectorOffset> chsToLba(const CHSAddress& chs, const CHSGeometry& geometry)
{
    if (geometry.headsPerCylinder == 0 || geometry.sectorsPerTrack == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "CHS geometry has zero heads or sectors per track");
    }

    // CHS sector numbers are 1-based; sector 0 is invalid
    if (chs.sector == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "CHS sector number must be >= 1 (1-based addressing)");
    }

    // LBA = (C * HPC * SPT) + (H * SPT) + (S - 1)
    uint64_t lba = static_cast<uint64_t>(chs.cylinder)
                   * geometry.headsPerCylinder
                   * geometry.sectorsPerTrack;
    lba += static_cast<uint64_t>(chs.head) * geometry.sectorsPerTrack;
    lba += static_cast<uint64_t>(chs.sector) - 1;

    return lba;
}

Result<CHSAddress> lbaToChs(SectorOffset lba, const CHSGeometry& geometry)
{
    if (geometry.headsPerCylinder == 0 || geometry.sectorsPerTrack == 0)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "CHS geometry has zero heads or sectors per track");
    }

    const uint64_t headsTimeSectors =
        static_cast<uint64_t>(geometry.headsPerCylinder) * geometry.sectorsPerTrack;

    CHSAddress result;
    result.cylinder = static_cast<uint32_t>(lba / headsTimeSectors);
    uint64_t remainder = lba % headsTimeSectors;
    result.head = static_cast<uint8_t>(remainder / geometry.sectorsPerTrack);
    // +1 because CHS sectors are 1-based
    result.sector = static_cast<uint8_t>((remainder % geometry.sectorsPerTrack) + 1);

    return result;
}

uint32_t normalizeSectorSize(uint32_t reportedSize)
{
    // Common physical sector sizes
    switch (reportedSize)
    {
    case 512:
    case 1024:
    case 2048:
    case 4096:
        return reportedSize;
    default:
        // If the reported size is a power of two and in a sane range, accept it
        if (reportedSize >= 512 && reportedSize <= 4096 &&
            (reportedSize & (reportedSize - 1)) == 0)
        {
            return reportedSize;
        }
        return DEFAULT_SECTOR_SIZE;
    }
}

bool isAligned(uint64_t byteOffset, uint64_t alignment)
{
    if (alignment == 0) return true;
    return (byteOffset % alignment) == 0;
}

bool isSectorAligned(SectorOffset lba, SectorCount alignmentSectors)
{
    if (alignmentSectors == 0) return true;
    return (lba % alignmentSectors) == 0;
}

uint64_t alignUp(uint64_t byteOffset, uint64_t alignment)
{
    if (alignment == 0) return byteOffset;
    const uint64_t remainder = byteOffset % alignment;
    if (remainder == 0) return byteOffset;
    return byteOffset + (alignment - remainder);
}

uint64_t alignDown(uint64_t byteOffset, uint64_t alignment)
{
    if (alignment == 0) return byteOffset;
    return byteOffset - (byteOffset % alignment);
}

SectorOffset alignSectorUp(SectorOffset lba, SectorCount alignmentSectors)
{
    if (alignmentSectors == 0) return lba;
    const SectorOffset remainder = lba % alignmentSectors;
    if (remainder == 0) return lba;
    return lba + (alignmentSectors - remainder);
}

SectorOffset alignSectorDown(SectorOffset lba, SectorCount alignmentSectors)
{
    if (alignmentSectors == 0) return lba;
    return lba - (lba % alignmentSectors);
}

uint64_t totalCapacity(SectorCount sectorCount, uint32_t sectorSize)
{
    return sectorCount * static_cast<uint64_t>(sectorSize);
}

SectorCount bytesToSectors(uint64_t bytes, uint32_t sectorSize)
{
    if (sectorSize == 0) return 0;
    return bytes / sectorSize;
}

SectorCount defaultAlignmentSectors(uint32_t sectorSize)
{
    if (sectorSize == 0) return 0;
    return DEFAULT_ALIGNMENT_BYTES / sectorSize;
}

SectorOffset optimalPartitionStart(SectorOffset desiredLba, uint32_t sectorSize)
{
    const SectorCount alignment = defaultAlignmentSectors(sectorSize);
    if (alignment == 0) return desiredLba;
    return alignSectorUp(desiredLba, alignment);
}

} // namespace DiskGeometry
} // namespace spw
