#pragma once

// DiskGeometry — CHS/LBA conversion, alignment checking, and capacity calculations.
// Reference: ATA/ATAPI Command Set (ACS-3), Section 6.2 for CHS addressing.

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../common/Constants.h"

#include <cstdint>

namespace spw
{

// Cylinder-Head-Sector address
struct CHSAddress
{
    uint32_t cylinder = 0;
    uint8_t head = 0;
    uint8_t sector = 0;    // 1-based (CHS sectors start at 1, not 0)
};

// Geometry parameters needed for CHS<->LBA conversion
struct CHSGeometry
{
    uint32_t headsPerCylinder = 0;
    uint32_t sectorsPerTrack = 0;
};

namespace DiskGeometry
{

// Convert CHS address to LBA.
// Formula: LBA = (C * HPC * SPT) + (H * SPT) + (S - 1)
// where HPC = heads per cylinder, SPT = sectors per track.
// Returns error if geometry parameters are zero (division by zero guard).
Result<SectorOffset> chsToLba(const CHSAddress& chs, const CHSGeometry& geometry);

// Convert LBA to CHS address.
// This is the inverse: C = LBA / (HPC * SPT), H = (LBA / SPT) % HPC, S = (LBA % SPT) + 1
Result<CHSAddress> lbaToChs(SectorOffset lba, const CHSGeometry& geometry);

// Detect the physical sector size from a given value.
// Returns SECTOR_SIZE_512 or SECTOR_SIZE_4K.
// Falls back to DEFAULT_SECTOR_SIZE if the value is not recognized.
uint32_t normalizeSectorSize(uint32_t reportedSize);

// Check if a byte offset is aligned to a given alignment boundary.
bool isAligned(uint64_t byteOffset, uint64_t alignment);

// Check if an LBA is aligned to a given sector count boundary.
bool isSectorAligned(SectorOffset lba, SectorCount alignmentSectors);

// Round a byte offset UP to the next alignment boundary.
// Returns the offset unchanged if it is already aligned.
uint64_t alignUp(uint64_t byteOffset, uint64_t alignment);

// Round a byte offset DOWN to the previous alignment boundary.
uint64_t alignDown(uint64_t byteOffset, uint64_t alignment);

// Round an LBA up to the next aligned sector.
SectorOffset alignSectorUp(SectorOffset lba, SectorCount alignmentSectors);

// Round an LBA down to the previous aligned sector.
SectorOffset alignSectorDown(SectorOffset lba, SectorCount alignmentSectors);

// Calculate total capacity in bytes from sector count and sector size.
uint64_t totalCapacity(SectorCount sectorCount, uint32_t sectorSize);

// Calculate the number of sectors that fit in a given byte count.
// Rounds DOWN — partial sectors are not counted.
SectorCount bytesToSectors(uint64_t bytes, uint32_t sectorSize);

// Calculate the default alignment in sectors for a given sector size.
// Uses DEFAULT_ALIGNMENT_BYTES (1 MiB).
SectorCount defaultAlignmentSectors(uint32_t sectorSize);

// Calculate optimal partition start for a given desired LBA, respecting alignment.
// Returns the next aligned LBA >= desiredLba.
SectorOffset optimalPartitionStart(SectorOffset desiredLba, uint32_t sectorSize);

} // namespace DiskGeometry
} // namespace spw
