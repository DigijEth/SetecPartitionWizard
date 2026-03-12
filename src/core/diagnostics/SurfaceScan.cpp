// SurfaceScan.cpp -- Bad sector detection via read / write-verify testing.
//
// DISCLAIMER: This code is for authorized disk utility software only.
//             Write-verify mode DESTROYS all data on the scanned area.

#include "SurfaceScan.h"

#include <algorithm>
#include <cstring>

namespace spw
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SurfaceScan::SurfaceScan(RawDiskHandle& disk)
    : m_disk(disk)
{
}

// ---------------------------------------------------------------------------
// scanDisk -- scan the entire physical disk
// ---------------------------------------------------------------------------

Result<SurfaceScanResults> SurfaceScan::scanDisk(
    SurfaceScanMode mode,
    SurfaceScanProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    auto geoResult = m_disk.getGeometry();
    if (geoResult.isError())
        return geoResult.error();

    const auto& geo = geoResult.value();
    const uint32_t sectorSize = geo.bytesPerSector;
    if (sectorSize == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Disk reports 0 bytes/sector");

    const uint64_t totalSectors = geo.totalBytes / sectorSize;
    return scanImpl(0, totalSectors, sectorSize, mode, progressCb, cancelFlag);
}

// ---------------------------------------------------------------------------
// scanRange -- scan a specific LBA range
// ---------------------------------------------------------------------------

Result<SurfaceScanResults> SurfaceScan::scanRange(
    SectorOffset startLba,
    SectorCount  sectorCount,
    SurfaceScanMode mode,
    SurfaceScanProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    auto geoResult = m_disk.getGeometry();
    if (geoResult.isError())
        return geoResult.error();

    const uint32_t sectorSize = geoResult.value().bytesPerSector;
    if (sectorSize == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Disk reports 0 bytes/sector");

    return scanImpl(startLba, sectorCount, sectorSize, mode, progressCb, cancelFlag);
}

// ---------------------------------------------------------------------------
// scanImpl -- core scan loop
//
// We read in chunks of 256 sectors (128 KiB at 512 bytes/sector) for
// throughput.  When a chunk fails, we fall back to reading individual
// sectors within that chunk to isolate the specific bad sector(s).
// ---------------------------------------------------------------------------

Result<SurfaceScanResults> SurfaceScan::scanImpl(
    SectorOffset startLba,
    SectorCount  sectorCount,
    uint32_t     sectorSize,
    SurfaceScanMode mode,
    SurfaceScanProgress progressCb,
    std::atomic<bool>* cancelFlag)
{
    if (sectorCount == 0)
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument, "Sector count is 0");

    SurfaceScanResults results;
    results.totalSectorsTested = 0;
    results.badSectorCount = 0;

    // Chunk size in sectors: read 256 sectors at a time
    const SectorCount chunkSectors = 256;

    // For write-verify mode, we use a pattern buffer
    // Pattern: alternating 0xAA and 0x55 bytes (checkerboard)
    const uint32_t chunkBytes = static_cast<uint32_t>(chunkSectors) * sectorSize;
    std::vector<uint8_t> writePattern(chunkBytes);
    for (size_t i = 0; i < writePattern.size(); ++i)
        writePattern[i] = (i % 2 == 0) ? 0xAA : 0x55;

    // Timing
    LARGE_INTEGER perfFreq, perfStart, perfNow;
    QueryPerformanceFrequency(&perfFreq);
    QueryPerformanceCounter(&perfStart);

    SectorOffset currentLba = startLba;
    SectorOffset endLba = startLba + sectorCount;

    while (currentLba < endLba)
    {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed))
            return ErrorInfo::fromCode(ErrorCode::OperationCanceled, "Surface scan canceled");

        SectorCount remaining = endLba - currentLba;
        SectorCount thisChunk = std::min(chunkSectors, remaining);

        bool chunkOk = true;

        if (mode == SurfaceScanMode::ReadOnly)
        {
            // Attempt to read the entire chunk
            auto readResult = m_disk.readSectors(currentLba, thisChunk, sectorSize);
            if (readResult.isError())
                chunkOk = false;
        }
        else // WriteVerify
        {
            // Write the pattern
            uint32_t patternSize = static_cast<uint32_t>(thisChunk) * sectorSize;
            auto writeResult = m_disk.writeSectors(currentLba, writePattern.data(),
                                                    thisChunk, sectorSize);
            if (writeResult.isError())
            {
                chunkOk = false;
            }
            else
            {
                // Read back and verify
                auto readResult = m_disk.readSectors(currentLba, thisChunk, sectorSize);
                if (readResult.isError())
                {
                    chunkOk = false;
                }
                else
                {
                    const auto& readData = readResult.value();
                    if (readData.size() < patternSize ||
                        std::memcmp(readData.data(), writePattern.data(), patternSize) != 0)
                    {
                        chunkOk = false;
                    }
                }
            }
        }

        if (!chunkOk)
        {
            // Chunk had an error. Fall back to testing individual sectors
            // to isolate which specific sectors are bad.
            for (SectorCount s = 0; s < thisChunk; ++s)
            {
                SectorOffset testLba = currentLba + s;
                BadSector bad;
                bad.lba = testLba;

                if (mode == SurfaceScanMode::ReadOnly)
                {
                    auto singleRead = m_disk.readSectors(testLba, 1, sectorSize);
                    if (singleRead.isError())
                    {
                        bad.readError = true;
                        results.badSectors.push_back(bad);
                        results.badSectorCount++;
                    }
                }
                else // WriteVerify
                {
                    // Write one sector
                    auto singleWrite = m_disk.writeSectors(testLba, writePattern.data(),
                                                            1, sectorSize);
                    if (singleWrite.isError())
                    {
                        bad.writeError = true;
                        results.badSectors.push_back(bad);
                        results.badSectorCount++;
                        continue;
                    }

                    // Read back
                    auto singleRead = m_disk.readSectors(testLba, 1, sectorSize);
                    if (singleRead.isError())
                    {
                        bad.readError = true;
                        results.badSectors.push_back(bad);
                        results.badSectorCount++;
                        continue;
                    }

                    // Verify
                    const auto& readData = singleRead.value();
                    if (readData.size() < sectorSize ||
                        std::memcmp(readData.data(), writePattern.data(), sectorSize) != 0)
                    {
                        bad.verifyError = true;
                        results.badSectors.push_back(bad);
                        results.badSectorCount++;
                    }
                }
            }
        }

        results.totalSectorsTested += thisChunk;
        currentLba += thisChunk;

        // Progress reporting
        if (progressCb)
        {
            QueryPerformanceCounter(&perfNow);
            double elapsed = static_cast<double>(perfNow.QuadPart - perfStart.QuadPart) /
                             static_cast<double>(perfFreq.QuadPart);

            double bytesScanned = static_cast<double>(results.totalSectorsTested) * sectorSize;
            double speedMBps = (elapsed > 0.0)
                ? (bytesScanned / (1024.0 * 1024.0)) / elapsed
                : 0.0;

            double sectorsRemaining = static_cast<double>(sectorCount - results.totalSectorsTested);
            double sectorsPerSec = (elapsed > 0.0)
                ? static_cast<double>(results.totalSectorsTested) / elapsed
                : 0.0;
            double etaSeconds = (sectorsPerSec > 0.0)
                ? sectorsRemaining / sectorsPerSec
                : 0.0;

            progressCb(results.totalSectorsTested,
                       sectorCount,
                       results.badSectorCount,
                       speedMBps,
                       etaSeconds);
        }
    }

    // Final timing
    QueryPerformanceCounter(&perfNow);
    results.elapsedSeconds = static_cast<double>(perfNow.QuadPart - perfStart.QuadPart) /
                             static_cast<double>(perfFreq.QuadPart);

    double totalMB = static_cast<double>(results.totalSectorsTested) * sectorSize / (1024.0 * 1024.0);
    results.averageSpeedMBps = (results.elapsedSeconds > 0.0)
        ? totalMB / results.elapsedSeconds
        : 0.0;

    return results;
}

} // namespace spw
