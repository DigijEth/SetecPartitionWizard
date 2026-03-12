#pragma once

// SurfaceScan -- Bad sector detection via read (and optional write-verify) testing.
//
// Reads every sector on a disk or partition and records any sectors that
// return I/O errors.  The optional write test (read-write-verify) is DESTRUCTIVE:
// it writes a known pattern, reads it back, and verifies the data matches.
//
// DISCLAIMER: This code is for authorized disk utility software only.
//             The write-verify test DESTROYS all data on the scanned area.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include "../common/Constants.h"
#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"
#include "../disk/RawDiskHandle.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spw
{

// Record of a single bad sector
struct BadSector
{
    SectorOffset lba = 0;
    bool readError  = false;  // Failed to read
    bool writeError = false;  // Failed to write (write-verify mode only)
    bool verifyError = false; // Read-back mismatch (write-verify mode only)
};

// Results of a surface scan
struct SurfaceScanResults
{
    uint64_t totalSectorsTested = 0;
    uint64_t badSectorCount = 0;
    double   elapsedSeconds = 0.0;
    double   averageSpeedMBps = 0.0;
    std::vector<BadSector> badSectors;
};

// Scan mode
enum class SurfaceScanMode
{
    ReadOnly,     // Non-destructive: read every sector
    WriteVerify,  // DESTRUCTIVE: write pattern, read back, verify
};

// Progress callback.
// Parameters: (sectorsScanned, totalSectors, badSectorsFound, currentSpeedMBps, etaSeconds)
using SurfaceScanProgress = std::function<void(uint64_t sectorsScanned,
                                                uint64_t totalSectors,
                                                uint64_t badSectors,
                                                double   speedMBps,
                                                double   etaSeconds)>;

class SurfaceScan
{
public:
    explicit SurfaceScan(RawDiskHandle& disk);

    // Scan the entire disk
    Result<SurfaceScanResults> scanDisk(
        SurfaceScanMode mode = SurfaceScanMode::ReadOnly,
        SurfaceScanProgress progressCb = nullptr,
        std::atomic<bool>* cancelFlag = nullptr);

    // Scan a specific partition (range of sectors)
    Result<SurfaceScanResults> scanRange(
        SectorOffset startLba,
        SectorCount  sectorCount,
        SurfaceScanMode mode = SurfaceScanMode::ReadOnly,
        SurfaceScanProgress progressCb = nullptr,
        std::atomic<bool>* cancelFlag = nullptr);

private:
    // Internal implementation shared by scanDisk and scanRange
    Result<SurfaceScanResults> scanImpl(
        SectorOffset startLba,
        SectorCount  sectorCount,
        uint32_t     sectorSize,
        SurfaceScanMode mode,
        SurfaceScanProgress progressCb,
        std::atomic<bool>* cancelFlag);

    RawDiskHandle& m_disk;
};

} // namespace spw
