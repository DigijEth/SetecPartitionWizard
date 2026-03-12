#pragma once

// PartitionRecovery -- Scan a physical disk for lost/deleted partition superblocks.
//
// Two scan modes:
//   Quick scan: checks 1 MiB alignment boundaries only (fast, finds most modern partitions).
//   Deep scan:  checks every sector (slow, finds everything including cylinder-aligned relics).
//
// Found partitions are cross-referenced against the existing partition table so that only
// genuinely missing entries are reported.
//
// DISCLAIMER: This code is for authorized disk utility software only.
//             Recovery writes modify the partition table -- always confirm with the user first.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "../common/Constants.h"
#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../disk/RawDiskHandle.h"
#include "../disk/PartitionTable.h"
#include "../disk/FilesystemDetector.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spw
{

// A partition candidate found during recovery scanning
struct RecoveredPartition
{
    SectorOffset startLba = 0;
    SectorCount  sectorCount = 0;
    uint32_t     sectorSize = SECTOR_SIZE_512;
    FilesystemType fsType = FilesystemType::Unknown;
    std::string  label;            // Volume label if readable from superblock
    double       confidence = 0.0; // 0.0 - 100.0
    bool         overlapsExisting = false;
};

// Scan mode
enum class PartitionScanMode
{
    Quick, // Every 1 MiB boundary
    Deep,  // Every sector
};

// Progress callback for partition recovery scan.
// Parameters: (sectorsScanned, totalSectors, partitionsFoundSoFar)
using PartitionScanProgress = std::function<void(uint64_t sectorsScanned,
                                                  uint64_t totalSectors,
                                                  size_t   partitionsFound)>;

class PartitionRecovery
{
public:
    explicit PartitionRecovery(RawDiskHandle& disk);

    // Run the scan. Results are returned as a vector of candidates.
    Result<std::vector<RecoveredPartition>> scan(
        PartitionScanMode mode,
        PartitionScanProgress progressCb = nullptr,
        std::atomic<bool>* cancelFlag = nullptr);

    // Write a recovered partition back to the partition table.
    // Works for both MBR and GPT.  The caller must have opened the disk ReadWrite.
    Result<void> recover(const RecoveredPartition& partition);

private:
    // Probe a single sector offset to see if a filesystem superblock starts there.
    // Returns an empty optional if nothing was found.
    bool probeOffset(SectorOffset lba, RecoveredPartition& out) const;

    // Determine partition size from the superblock at the given LBA.
    uint64_t estimatePartitionSize(SectorOffset lba, FilesystemType fs) const;

    // Cross-reference found partitions against existing table.
    void markOverlaps(std::vector<RecoveredPartition>& results) const;

    RawDiskHandle& m_disk;
    DiskGeometryInfo m_geometry = {};
    DriveLayoutInfo  m_layout   = {};
};

} // namespace spw
